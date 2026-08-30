#include <molto/util/json_write.h>

/* Two spaces per level. A document meant to be committed and diffed is read by
   people, and 80 columns of leading tabs is not read by anyone. */
#define INDENT_WIDTH 2

/* --- the destination --- */

static void sink_putc(json_sink *sink, char c) {
    if(sink->stream != NULL) {
        fputc(c, sink->stream);
        return;
    }
    /* One byte is always kept back for the terminator, which is what makes the
       buffer readable at any moment rather than only when finished. */
    if(sink->used + 1 >= sink->size) {
        sink->overflow = true;
        return;
    }
    sink->buffer[sink->used++] = c;
    sink->buffer[sink->used] = '\0';
}

static void sink_puts(json_sink *sink, const char *text) {
    for(const char *c = text; *c != '\0'; c++)
        sink_putc(sink, *c);
}

/* The one place a number is formatted, and it is always four hex digits. */
static void sink_escape_code(json_sink *sink, unsigned char value) {
    char escaped[7];
    snprintf(escaped, sizeof escaped, "\\u%04x", value);
    sink_puts(sink, escaped);
}

void json_writer_init(json_writer *writer, FILE *stream) {
    writer->sink = (json_sink){.stream = stream};
    writer->depth = 0;
    writer->pending_comma = false;
    writer->empty[0] = true;
}

void json_writer_init_buffer(json_writer *writer, char *buffer, size_t size) {
    writer->sink = (json_sink){.buffer = buffer, .size = size};
    if(size > 0)
        buffer[0] = '\0';
    else
        writer->sink.overflow = true;
    writer->depth = 0;
    writer->pending_comma = false;
    writer->empty[0] = true;
}

bool json_writer_overflowed(const json_writer *writer) { return writer->sink.overflow; }

void json_writer_finish(json_writer *writer) { sink_putc(&writer->sink, '\n'); }

static void string_to_sink(json_sink *sink, const char *text) {
    sink_putc(sink, '"');
    for(const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        switch(*p) {
        case '"':
            sink_puts(sink, "\\\"");
            break;
        case '\\':
            sink_puts(sink, "\\\\");
            break;
        case '\n':
            sink_puts(sink, "\\n");
            break;
        case '\r':
            sink_puts(sink, "\\r");
            break;
        case '\t':
            sink_puts(sink, "\\t");
            break;
        default:
            /* Everything below a space has to leave as an escape; a raw one is
               a document no parser will take. */
            if(*p < 0x20)
                sink_escape_code(sink, *p);
            else
                sink_putc(sink, (char)*p);
            break;
        }
    }
    sink_putc(sink, '"');
}

/* The public escaper still takes a stream, because its two other callers hand
   it one directly and have no writer to offer. */
void json_write_string(FILE *stream, const char *text) {
    json_sink sink = {.stream = stream};
    string_to_sink(&sink, text);
}

/* Whether the container currently open has had anything written into it.
   Levels past the limit report "not empty", which costs a newline in a
   document nobody has and keeps the brackets balanced. */
static bool current_is_empty(const json_writer *writer) {
    return writer->depth < JSON_WRITE_MAX_DEPTH ? writer->empty[writer->depth] : false;
}

static void mark_current_used(json_writer *writer) {
    if(writer->depth > 0 && writer->depth < JSON_WRITE_MAX_DEPTH)
        writer->empty[writer->depth] = false;
}

static void write_indent(json_writer *writer) {
    sink_putc(&writer->sink, '\n');
    for(int i = 0; i < writer->depth * INDENT_WIDTH; i++)
        sink_putc(&writer->sink, ' ');
}

/* Everything that comes before a value: the comma the previous one earned, the
   line it goes on, and the key when it has one. */
static void begin_value(json_writer *writer, const char *key) {
    if(writer->pending_comma)
        sink_putc(&writer->sink, ',');
    if(writer->depth > 0)
        write_indent(writer);
    mark_current_used(writer);

    if(key != NULL) {
        string_to_sink(&writer->sink, key);
        sink_puts(&writer->sink, ": ");
    }
}

static void open_container(json_writer *writer, const char *key, char bracket) {
    begin_value(writer, key);
    sink_putc(&writer->sink, bracket);

    writer->depth++;
    if(writer->depth < JSON_WRITE_MAX_DEPTH)
        writer->empty[writer->depth] = true;
    writer->pending_comma = false;
}

/* An empty container closes against its own bracket — `[]` — and a full one on
   the line of whatever opened it. */
static void close_container(json_writer *writer, char bracket) {
    const bool was_empty = current_is_empty(writer);
    if(writer->depth > 0)
        writer->depth--;
    if(!was_empty)
        write_indent(writer);

    sink_putc(&writer->sink, bracket);
    writer->pending_comma = true;
}

void json_object_open(json_writer *writer, const char *key) { open_container(writer, key, '{'); }

void json_object_close(json_writer *writer) { close_container(writer, '}'); }

void json_array_open(json_writer *writer, const char *key) { open_container(writer, key, '['); }

void json_array_close(json_writer *writer) { close_container(writer, ']'); }

void json_write_field(json_writer *writer, const char *key, const char *value) {
    begin_value(writer, key);
    string_to_sink(&writer->sink, value);
    writer->pending_comma = true;
}

void json_write_element(json_writer *writer, const char *value) {
    json_write_field(writer, NULL, value);
}

void json_write_raw(json_writer *writer, const char *key, const char *literal) {
    begin_value(writer, key);
    sink_puts(&writer->sink, literal);
    writer->pending_comma = true;
}
