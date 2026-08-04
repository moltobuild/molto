#include <molto/util/json.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Marks "no node" and "no text", so zero stays a usable index and offset. */
#define NO_INDEX SIZE_MAX

/* How the node array and the text pool start and grow. */
#define NODES_INITIAL 16
#define TEXT_INITIAL 256
#define GROWTH_FACTOR 2

/* Ranges of the UTF-16 surrogate pair a \u escape may encode. */
#define SURROGATE_HIGH_FIRST 0xD800u
#define SURROGATE_HIGH_LAST 0xDBFFu
#define SURROGATE_LOW_FIRST 0xDC00u
#define SURROGATE_LOW_LAST 0xDFFFu

typedef struct {
    json_type type;
    size_t text; /* offset into the text pool, or NO_INDEX */
    size_t key;  /* offset of a member's name, or NO_INDEX */
    bool boolean;
    size_t first_child;
    size_t last_child; /* kept so appending stays O(1) */
    size_t next_sibling;
    size_t child_count;
} json_node;

struct json_document {
    json_node *nodes;
    size_t node_count;
    size_t node_capacity;
    char *text;
    size_t text_used;
    size_t text_capacity;
};

/* --- document storage --- */

static bool grow_nodes(json_document *doc) {
    size_t next = doc->node_capacity == 0 ? NODES_INITIAL : doc->node_capacity * GROWTH_FACTOR;
    json_node *nodes = realloc(doc->nodes, next * sizeof *nodes);
    if(nodes == NULL)
        return false;
    doc->nodes = nodes;
    doc->node_capacity = next;
    return true;
}

/* Append a node of `type` and hand back its index, or NO_INDEX. */
static size_t add_node(json_document *doc, json_type type) {
    if(doc->node_count == doc->node_capacity && !grow_nodes(doc))
        return NO_INDEX;
    json_node *node = &doc->nodes[doc->node_count];
    *node = (json_node){
        .type = type,
        .text = NO_INDEX,
        .key = NO_INDEX,
        .first_child = NO_INDEX,
        .last_child = NO_INDEX,
        .next_sibling = NO_INDEX,
    };
    return doc->node_count++;
}

static bool grow_text(json_document *doc, size_t needed) {
    size_t next = doc->text_capacity == 0 ? TEXT_INITIAL : doc->text_capacity;
    while(next < needed)
        next *= GROWTH_FACTOR;
    char *text = realloc(doc->text, next);
    if(text == NULL)
        return false;
    doc->text = text;
    doc->text_capacity = next;
    return true;
}

/* Append one byte to the text pool. Offsets, not pointers, are handed out, so
   a reallocation here never invalidates anything already recorded. */
static bool push_byte(json_document *doc, char byte) {
    if(doc->text_used + 1 > doc->text_capacity && !grow_text(doc, doc->text_used + 1))
        return false;
    doc->text[doc->text_used++] = byte;
    return true;
}

/* --- parser --- */

typedef struct {
    const char *cursor;
    json_document *doc;
    int depth;
} parser;

static bool parse_value(parser *state, size_t *out);

static void skip_whitespace(parser *state) {
    while(*state->cursor == ' ' || *state->cursor == '\t' || *state->cursor == '\n' ||
          *state->cursor == '\r')
        state->cursor++;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Read four hex digits of a \u escape. */
static bool read_hex4(parser *state, unsigned *out) {
    unsigned value = 0;
    for(int i = 0; i < 4; i++) {
        char c = state->cursor[i];
        unsigned digit;
        if(is_digit(c))
            digit = (unsigned)(c - '0');
        else if(c >= 'a' && c <= 'f')
            digit = (unsigned)(c - 'a') + 10;
        else if(c >= 'A' && c <= 'F')
            digit = (unsigned)(c - 'A') + 10;
        else
            return false;
        value = value * 16 + digit;
    }
    state->cursor += 4;
    *out = value;
    return true;
}

/* Encode one code point as UTF-8 into the text pool. */
static bool push_utf8(json_document *doc, unsigned code) {
    if(code < 0x80u)
        return push_byte(doc, (char)code);
    if(code < 0x800u)
        return push_byte(doc, (char)(0xC0u | (code >> 6))) &&
               push_byte(doc, (char)(0x80u | (code & 0x3Fu)));
    if(code < 0x10000u)
        return push_byte(doc, (char)(0xE0u | (code >> 12))) &&
               push_byte(doc, (char)(0x80u | ((code >> 6) & 0x3Fu))) &&
               push_byte(doc, (char)(0x80u | (code & 0x3Fu)));
    return push_byte(doc, (char)(0xF0u | (code >> 18))) &&
           push_byte(doc, (char)(0x80u | ((code >> 12) & 0x3Fu))) &&
           push_byte(doc, (char)(0x80u | ((code >> 6) & 0x3Fu))) &&
           push_byte(doc, (char)(0x80u | (code & 0x3Fu)));
}

/* Decode a \uXXXX escape, joining a surrogate pair into the one code point it
   stands for. An unpaired surrogate is kept as-is rather than rejected: the
   text is data being reported, not something Molto interprets. */
static bool parse_unicode_escape(parser *state) {
    unsigned code;
    if(!read_hex4(state, &code))
        return false;

    if(code >= SURROGATE_HIGH_FIRST && code <= SURROGATE_HIGH_LAST && state->cursor[0] == '\\' &&
       state->cursor[1] == 'u') {
        const char *rewind = state->cursor;
        state->cursor += 2;
        unsigned low;
        if(read_hex4(state, &low) && low >= SURROGATE_LOW_FIRST && low <= SURROGATE_LOW_LAST)
            code = 0x10000u + ((code - SURROGATE_HIGH_FIRST) << 10) + (low - SURROGATE_LOW_FIRST);
        else
            state->cursor = rewind;
    }
    return push_utf8(state->doc, code);
}

static bool parse_escape(parser *state) {
    char escape = *state->cursor++;
    switch(escape) {
    case '"':
        return push_byte(state->doc, '"');
    case '\\':
        return push_byte(state->doc, '\\');
    case '/':
        return push_byte(state->doc, '/');
    case 'b':
        return push_byte(state->doc, '\b');
    case 'f':
        return push_byte(state->doc, '\f');
    case 'n':
        return push_byte(state->doc, '\n');
    case 'r':
        return push_byte(state->doc, '\r');
    case 't':
        return push_byte(state->doc, '\t');
    case 'u':
        return parse_unicode_escape(state);
    default:
        return false;
    }
}

/* Read a quoted string into the text pool and report where it landed. */
static bool parse_string(parser *state, size_t *out_offset) {
    if(*state->cursor != '"')
        return false;
    state->cursor++;

    size_t start = state->doc->text_used;
    while(*state->cursor != '"') {
        unsigned char c = (unsigned char)*state->cursor;
        if(c == '\0')
            return false; /* unterminated */
        if(c < 0x20)
            return false; /* raw control character */
        if(c == '\\') {
            state->cursor++;
            if(!parse_escape(state))
                return false;
            continue;
        }
        if(!push_byte(state->doc, (char)c))
            return false;
        state->cursor++;
    }
    state->cursor++;

    if(!push_byte(state->doc, '\0'))
        return false;
    *out_offset = start;
    return true;
}

/* Validate a JSON number and copy its text; the conversion happens on read, so
   an integer too large to represent is refused there rather than silently
   truncated here. */
static bool parse_number(parser *state, size_t *out_offset) {
    const char *start = state->cursor;

    if(*state->cursor == '-')
        state->cursor++;
    if(!is_digit(*state->cursor))
        return false;
    if(*state->cursor == '0')
        state->cursor++;
    else
        while(is_digit(*state->cursor))
            state->cursor++;

    if(*state->cursor == '.') {
        state->cursor++;
        if(!is_digit(*state->cursor))
            return false;
        while(is_digit(*state->cursor))
            state->cursor++;
    }
    if(*state->cursor == 'e' || *state->cursor == 'E') {
        state->cursor++;
        if(*state->cursor == '+' || *state->cursor == '-')
            state->cursor++;
        if(!is_digit(*state->cursor))
            return false;
        while(is_digit(*state->cursor))
            state->cursor++;
    }

    size_t offset = state->doc->text_used;
    for(const char *c = start; c < state->cursor; c++) {
        if(!push_byte(state->doc, *c))
            return false;
    }
    if(!push_byte(state->doc, '\0'))
        return false;
    *out_offset = offset;
    return true;
}

/* Match a bare word such as true, false or null. */
static bool match_literal(parser *state, const char *word) {
    size_t length = strlen(word);
    if(strncmp(state->cursor, word, length) != 0)
        return false;
    state->cursor += length;
    return true;
}

static void append_child(json_document *doc, size_t parent, size_t child) {
    json_node *owner = &doc->nodes[parent];
    if(owner->first_child == NO_INDEX)
        owner->first_child = child;
    else
        doc->nodes[owner->last_child].next_sibling = child;
    owner->last_child = child;
    owner->child_count++;
}

static bool parse_array(parser *state, size_t *out) {
    size_t array = add_node(state->doc, json_type_array);
    if(array == NO_INDEX)
        return false;
    state->cursor++; /* [ */

    skip_whitespace(state);
    if(*state->cursor == ']') {
        state->cursor++;
        *out = array;
        return true;
    }

    for(;;) {
        size_t element;
        if(!parse_value(state, &element))
            return false;
        append_child(state->doc, array, element);

        skip_whitespace(state);
        if(*state->cursor == ',') {
            state->cursor++;
            continue;
        }
        if(*state->cursor == ']') {
            state->cursor++;
            *out = array;
            return true;
        }
        return false;
    }
}

static bool parse_member(parser *state, size_t object) {
    size_t key;
    skip_whitespace(state);
    if(!parse_string(state, &key))
        return false;

    skip_whitespace(state);
    if(*state->cursor != ':')
        return false;
    state->cursor++;

    size_t value;
    if(!parse_value(state, &value))
        return false;
    state->doc->nodes[value].key = key;
    append_child(state->doc, object, value);
    return true;
}

static bool parse_object(parser *state, size_t *out) {
    size_t object = add_node(state->doc, json_type_object);
    if(object == NO_INDEX)
        return false;
    state->cursor++; /* { */

    skip_whitespace(state);
    if(*state->cursor == '}') {
        state->cursor++;
        *out = object;
        return true;
    }

    for(;;) {
        if(!parse_member(state, object))
            return false;

        skip_whitespace(state);
        if(*state->cursor == ',') {
            state->cursor++;
            continue;
        }
        if(*state->cursor == '}') {
            state->cursor++;
            *out = object;
            return true;
        }
        return false;
    }
}

/* Add a leaf node carrying `text`, which the caller already put in the pool. */
static bool add_leaf(parser *state, json_type type, size_t text, size_t *out) {
    size_t node = add_node(state->doc, type);
    if(node == NO_INDEX)
        return false;
    state->doc->nodes[node].text = text;
    *out = node;
    return true;
}

static bool add_bool(parser *state, bool value, size_t *out) {
    size_t node = add_node(state->doc, json_type_bool);
    if(node == NO_INDEX)
        return false;
    state->doc->nodes[node].boolean = value;
    *out = node;
    return true;
}

static bool parse_value(parser *state, size_t *out) {
    if(state->depth >= JSON_MAX_DEPTH)
        return false;

    skip_whitespace(state);
    switch(*state->cursor) {
    case '{': {
        state->depth++;
        bool ok = parse_object(state, out);
        state->depth--;
        return ok;
    }
    case '[': {
        state->depth++;
        bool ok = parse_array(state, out);
        state->depth--;
        return ok;
    }
    case '"': {
        size_t text;
        return parse_string(state, &text) && add_leaf(state, json_type_string, text, out);
    }
    case 't':
        return match_literal(state, "true") && add_bool(state, true, out);
    case 'f':
        return match_literal(state, "false") && add_bool(state, false, out);
    case 'n':
        return match_literal(state, "null") && add_leaf(state, json_type_null, NO_INDEX, out);
    default: {
        size_t text;
        return parse_number(state, &text) && add_leaf(state, json_type_number, text, out);
    }
    }
}

json_document *json_parse(const char *text) {
    if(text == NULL)
        return NULL;

    json_document *doc = calloc(1, sizeof *doc);
    if(doc == NULL)
        return NULL;

    parser state = {.cursor = text, .doc = doc, .depth = 0};
    size_t root;
    if(!parse_value(&state, &root)) {
        json_free(doc);
        return NULL;
    }

    /* Trailing content means the text was not one document, and accepting it
       would hide a truncated or concatenated response. */
    skip_whitespace(&state);
    if(*state.cursor != '\0') {
        json_free(doc);
        return NULL;
    }
    return doc;
}

void json_free(json_document *doc) {
    if(doc == NULL)
        return;
    free(doc->nodes);
    free(doc->text);
    free(doc);
}

/* --- reading --- */

static const json_node *node_of(json_value value) {
    if(value.doc == NULL || value.index >= value.doc->node_count)
        return NULL;
    return &value.doc->nodes[value.index];
}

static json_value invalid_value(void) { return (json_value){.doc = NULL, .index = NO_INDEX}; }

static json_value value_at(const json_document *doc, size_t index) {
    return (json_value){.doc = doc, .index = index};
}

json_value json_root(const json_document *doc) {
    if(doc == NULL || doc->node_count == 0)
        return invalid_value();
    return value_at(doc, 0);
}

bool json_is_valid(json_value value) { return node_of(value) != NULL; }

json_type json_type_of(json_value value) {
    const json_node *node = node_of(value);
    return node != NULL ? node->type : json_type_invalid;
}

json_value json_get(json_value object, const char *key) {
    const json_node *node = node_of(object);
    if(node == NULL || node->type != json_type_object || key == NULL)
        return invalid_value();

    for(size_t i = node->first_child; i != NO_INDEX; i = object.doc->nodes[i].next_sibling) {
        size_t offset = object.doc->nodes[i].key;
        if(offset != NO_INDEX && strcmp(object.doc->text + offset, key) == 0)
            return value_at(object.doc, i);
    }
    return invalid_value();
}

json_value json_at(json_value array, size_t index) {
    const json_node *node = node_of(array);
    if(node == NULL || node->type != json_type_array)
        return invalid_value();

    size_t position = 0;
    for(size_t i = node->first_child; i != NO_INDEX; i = array.doc->nodes[i].next_sibling) {
        if(position++ == index)
            return value_at(array.doc, i);
    }
    return invalid_value();
}

size_t json_count(json_value value) {
    const json_node *node = node_of(value);
    if(node == NULL || (node->type != json_type_array && node->type != json_type_object))
        return 0;
    return node->child_count;
}

const char *json_string(json_value value) {
    const json_node *node = node_of(value);
    if(node == NULL || node->type != json_type_string)
        return NULL;
    return value.doc->text + node->text;
}

bool json_number(json_value value, long long *out) {
    const json_node *node = node_of(value);
    if(node == NULL || node->type != json_type_number)
        return false;

    const char *text = value.doc->text + node->text;
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    /* A number too large saturates instead of failing, so ERANGE is what tells
       the two apart; reporting a saturated value would be worse than refusing. */
    if(end == text || *end != '\0' || errno == ERANGE)
        return false; /* fractional, exponential, or out of range */
    *out = parsed;
    return true;
}

bool json_bool(json_value value, bool *out) {
    const json_node *node = node_of(value);
    if(node == NULL || node->type != json_type_bool)
        return false;
    *out = node->boolean;
    return true;
}

/* Walk to member `index` of an object. An object is a sibling chain, so getting
   to the nth member means counting to it, and the two accessors below would
   otherwise carry the same walk twice. */
static size_t member_index(json_value object, size_t index) {
    const json_node *node = node_of(object);
    if(node == NULL || node->type != json_type_object)
        return NO_INDEX;

    size_t position = 0;
    for(size_t i = node->first_child; i != NO_INDEX; i = object.doc->nodes[i].next_sibling) {
        if(position++ == index)
            return i;
    }
    return NO_INDEX;
}

const char *json_key_at(json_value object, size_t index) {
    size_t found = member_index(object, index);
    if(found == NO_INDEX)
        return NULL;
    size_t offset = object.doc->nodes[found].key;
    return offset != NO_INDEX ? object.doc->text + offset : NULL;
}

json_value json_member_at(json_value object, size_t index) {
    size_t found = member_index(object, index);
    return found != NO_INDEX ? value_at(object.doc, found) : invalid_value();
}
