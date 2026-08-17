#include <molto/build/diagnostic_view.h>

#include <molto/services/fs_service.h>
#include <molto/util/ansi.h>
#include <molto/util/text.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* How many diagnostics get a frame before the rest fall back to one line each.
   A missing semicolon in C cascades into dozens of errors, and forty frames is
   not more readable than forty lines — it is the same information spread over
   a screenful nobody will scroll back through. Nothing is dropped: past this
   count the normalized form still says everything the frame would have. */
#define FRAME_LIMIT 10

/* Columns reserved for a line number. Two is what fits the common case without
   the frame drifting rightwards on a file with four-digit lines. */
#define GUTTER_MIN 2

/* How deep the header and the summary line sit. The frame indents itself by
   its own gutter, which is usually the same and occasionally deeper. */
#define BLOCK_INDENT 3

/* How many "In file included from" steps are carried into a frame. A chain
   longer than this is a template header pulling in the world, and the last
   steps are the ones that name the reader's own file. */
#define CHAIN_MAX 4

#define GLYPH_FAILED "✗"
#define GLYPH_WARNED "⚠"
#define RULE_CORNER "┌─"
#define RULE_TEE "├─"
#define RULE_BAR "│"

/* --- a string that grows --- */

typedef struct {
    char *data;
    size_t used;
    size_t capacity;
    bool failed;
} view_text;

static void text_init(view_text *text) { *text = (view_text){0}; }

static void text_free(view_text *text) {
    free(text->data);
    text_init(text);
}

static bool text_reserve(view_text *text, size_t extra) {
    if(text->used + extra <= text->capacity)
        return true;
    size_t capacity = text->capacity == 0 ? 1024 : text->capacity;
    while(capacity < text->used + extra)
        capacity *= 2;
    char *grown = realloc(text->data, capacity);
    if(grown == NULL)
        return false;
    text->data = grown;
    text->capacity = capacity;
    return true;
}

static void text_add(view_text *text, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void text_add(view_text *text, const char *format, ...) {
    if(text->failed)
        return;
    va_list probe;
    va_list args;
    va_start(probe, format);
    va_copy(args, probe);
    const int needed = vsnprintf(NULL, 0, format, probe);
    va_end(probe);
    if(needed < 0 || !text_reserve(text, (size_t)needed + 1)) {
        text->failed = true;
        va_end(args);
        return;
    }
    (void)vsnprintf(text->data + text->used, (size_t)needed + 1, format, args);
    va_end(args);
    text->used += (size_t)needed;
}

/* --- colour --- */

/* Resolved once, into strings that are empty when colour is not wanted, so no
   format string below has to branch on it. */
typedef struct {
    const char *reset;
    const char *bold;
    const char *dim;
    const char *severity; /* red for an error, yellow for a warning */
} view_style;

static view_style style_for(bool colour, bool failed) {
    if(!colour)
        return (view_style){"", "", "", ""};
    return (view_style){ANSI_RESET, ANSI_BOLD, ANSI_DIM, failed ? ANSI_RED : ANSI_YELLOW};
}

/* --- reading what the compiler wrote --- */

/* Copy `in` without its escape sequences.
 *
 * Capturing through a pipe already turns a compiler's colour off, so anything
 * that arrives coloured was asked for explicitly in the project's flags. Those
 * escapes are dropped rather than passed through: they were chosen to stand
 * out against a plain stream and they fight a frame that colours itself. */
static void strip_csi(const char *in, char *out, size_t out_size) {
    size_t used = 0;
    const char *at = in;
    while(*at != '\0' && used + 1 < out_size) {
        if(at[0] == '\033' && at[1] == '[') {
            at += 2;
            while(*at != '\0' && (*at < '@' || *at > '~'))
                at++;
            if(*at != '\0')
                at++;
            continue;
        }
        out[used++] = *at++;
    }
    out[used] = '\0';
}

/* Whether this is the line a compiler draws under a source line.
 *
 * Recognised on the page rather than suppressed at the command line: gcc and
 * clang spell that flag differently, clang-tidy accepts neither, and any flag
 * added to a compile line lands in the fingerprint that decides whether an
 * object is stale — so asking for it would rebuild every object on the machine
 * to change how a message looks. A caret line, meanwhile, is made of nothing
 * but the marks, the spaces before them and the gutter gcc puts them in. */
static bool is_caret_line(const diagnostic *item) {
    if(item->severity != diagnostic_severity_unknown)
        return false;
    char plain[DIAGNOSTIC_TEXT_MAX];
    strip_csi(item->message, plain, sizeof plain);

    bool marked = false;
    for(const char *at = plain; *at != '\0'; at++) {
        if(*at == '^' || *at == '~') {
            marked = true;
            continue;
        }
        if(*at == ' ' || *at == '|' || (*at >= '0' && *at <= '9'))
            continue;
        return false;
    }
    return marked;
}

/* The location out of an "In file included from a.h:1," step, or the
   "                 from m.c:1:" that continues one. These come before the
   diagnostic they explain, and they are what names the file of the reader's
   own that reached the header which broke. */
static bool included_from(const diagnostic *item, char *out, size_t out_size) {
    if(item->severity != diagnostic_severity_unknown)
        return false;
    char plain[DIAGNOSTIC_TEXT_MAX];
    strip_csi(item->message, plain, sizeof plain);

    const char *at = plain;
    while(*at == ' ')
        at++;
    if(strncmp(at, "In file included from ", 22) == 0)
        at += 22;
    else if(strncmp(at, "from ", 5) == 0)
        at += 5;
    else
        return false;

    /* The chain separates its steps with a comma and ends with a colon. */
    size_t length = strlen(at);
    while(length > 0 && (at[length - 1] == ',' || at[length - 1] == ':'))
        length--;
    if(length == 0 || length + 1 > out_size)
        return false;
    memcpy(out, at, length);
    out[length] = '\0';
    return true;
}

/* gcc announces which function it is inside before the diagnostics in it:
   "src/database.c: In function 'lookup':". clang says no such thing, and the
   locator below already names the file and the line, so keeping it would make
   the two compilers read differently to say the same amount. */
static bool is_enclosing_context(const diagnostic *item) {
    if(item->severity != diagnostic_severity_unknown)
        return false;
    char plain[DIAGNOSTIC_TEXT_MAX];
    strip_csi(item->message, plain, sizeof plain);
    const size_t length = strlen(plain);
    if(length == 0 || plain[length - 1] != ':')
        return false;
    return strstr(plain, ": In ") != NULL || strstr(plain, ": At ") != NULL;
}

static bool has_location(const diagnostic *item) { return item->file[0] != '\0' && item->line > 0; }

/* Whether this diagnostic opens a frame. A note continues the one above it. */
static bool opens_a_frame(const diagnostic *item) {
    return item->severity == diagnostic_severity_error ||
           item->severity == diagnostic_severity_warning;
}

/* --- deciding what survives --- */

/* One flag per diagnostic: false for the excerpt and caret a compiler drew,
   which this file draws again and better. The caret is what identifies the
   pair, so the line before it goes too. */
static bool *keep_flags(const diagnostic_list *list) {
    bool *keep = malloc(list->count * sizeof *keep);
    if(keep == NULL)
        return NULL;
    for(size_t i = 0; i < list->count; i++)
        keep[i] = true;
    for(size_t i = 0; i < list->count; i++) {
        if(!is_caret_line(&list->items[i]))
            continue;
        keep[i] = false;
        if(i > 0 && list->items[i - 1].severity == diagnostic_severity_unknown)
            keep[i - 1] = false;
    }
    return keep;
}

/* Whether anything survived that is worth a block of its own. A compiler that
   said only which function it was in said nothing. */
static bool anything_to_draw(const diagnostic_list *list, const bool *keep) {
    for(size_t i = 0; i < list->count; i++) {
        const diagnostic *item = &list->items[i];
        if(keep[i] && item->message[0] != '\0' && !is_enclosing_context(item))
            return true;
    }
    return false;
}

static bool any_error(const diagnostic_list *list, const bool *keep) {
    for(size_t i = 0; i < list->count; i++) {
        if(keep[i] && list->items[i].severity == diagnostic_severity_error)
            return true;
    }
    return false;
}

/* Digits in the widest line number a frame will show, so every rule in the
   block lines up under the same column. */
static size_t gutter_width(const diagnostic_list *list, const bool *keep) {
    long widest = 0;
    for(size_t i = 0; i < list->count; i++) {
        if(keep[i] && list->items[i].line > widest)
            widest = list->items[i].line;
    }
    size_t digits = 1;
    for(long at = widest; at >= 10; at /= 10)
        digits++;
    return digits < GUTTER_MIN ? GUTTER_MIN : digits;
}

/* --- drawing --- */

typedef struct {
    view_text *text;
    const view_style *style;
    const diagnostic_context *ctx;
    size_t gutter;
    size_t frames;
    char chain[CHAIN_MAX][DIAGNOSTIC_PATH_MAX];
    size_t chain_count;
} view_state;

static const char *shown_path(const view_state *view, const char *path) {
    return view->ctx->root != NULL ? fs_relative_to(path, view->ctx->root) : path;
}

/* An empty rule: the frame's own vertical, with nothing beside it. */
static void write_bar(view_state *view) {
    text_add(view->text, "%s%*s %s%s\n", view->style->dim, (int)view->gutter, "", RULE_BAR,
             view->style->reset);
}

/* The rule that opens a frame, and the chain of includes that led to it. */
static void write_locator(view_state *view, const diagnostic *item, const char *corner,
                          const char *label) {
    const char *path = shown_path(view, item->file);
    if(item->column > 0)
        text_add(view->text, "%s%*s %s %s%s%s:%ld:%ld\n", view->style->dim, (int)view->gutter, "",
                 corner, label, view->style->reset, path, item->line, item->column);
    else
        text_add(view->text, "%s%*s %s %s%s%s:%ld\n", view->style->dim, (int)view->gutter, "",
                 corner, label, view->style->reset, path, item->line);

    for(size_t i = 0; i < view->chain_count; i++)
        text_add(view->text, "%s%*s %s  included from %s%s\n", view->style->dim, (int)view->gutter,
                 "", RULE_BAR, view->chain[i], view->style->reset);
}

/* The source line the compiler pointed at, with tabs spent one column each so
   the byte the compiler named and the column it lands in stay the same number. */
static bool write_excerpt(view_state *view, const diagnostic *item, char *line, size_t line_size) {
    if(!fs_read_line(item->file, item->line, line, line_size))
        return false;
    for(char *at = line; *at != '\0'; at++) {
        if(*at == '\t')
            *at = ' ';
    }
    text_add(view->text, "%s%*ld %s%s %s\n", view->style->dim, (int)view->gutter, item->line,
             RULE_BAR, view->style->reset, line);
    return true;
}

/* The caret, under the character the compiler named, carrying what it said. */
static void write_caret(view_state *view, const diagnostic *item, const char *line) {
    const size_t at = item->column > 0 ? text_columns(line, (size_t)item->column - 1) : 0;
    char message[DIAGNOSTIC_TEXT_MAX];
    strip_csi(item->message, message, sizeof message);
    text_add(view->text, "%s%*s %s%s %*s%s^%s %s\n", view->style->dim, (int)view->gutter, "",
             RULE_BAR, view->style->reset, (int)at, "", view->style->severity, view->style->reset,
             message);
}

/* What a frame says when the source cannot be read, or when the compiler named
   no column to point at. The first is the ordinary case for `<command line>`
   and for anything generated and since removed. */
static void write_message_beside_the_bar(view_state *view, const diagnostic *item) {
    char message[DIAGNOSTIC_TEXT_MAX];
    strip_csi(item->message, message, sizeof message);
    text_add(view->text, "%s%*s %s%s %s\n", view->style->dim, (int)view->gutter, "", RULE_BAR,
             view->style->reset, message);
}

/* One diagnostic, framed. `corner` opens a new box or continues the one above,
   which is how a note stays attached to the error it explains. */
static void write_frame(view_state *view, const diagnostic *item, const char *corner,
                        const char *label) {
    write_locator(view, item, corner, label);
    view->chain_count = 0;
    write_bar(view);

    char line[DIAGNOSTIC_TEXT_MAX];
    if(write_excerpt(view, item, line, sizeof line) && item->column > 0)
        write_caret(view, item, line);
    else
        write_message_beside_the_bar(view, item);
    write_bar(view);
}

/* The line above a frame: the severity, the compiler's own rule where it named
   one, and what it meant for the build. */
static void write_summary(view_state *view, const diagnostic *item) {
    static const char *const outcomes[][2] = {
        [diagnostic_view_compiling] = {"compiled with a warning", "compilation failed"},
        [diagnostic_view_linking] = {"linked with a warning", "linking failed"},
    };
    const char *word = diagnostic_severity_name(item->severity);
    const bool failed = item->severity == diagnostic_severity_error;
    const char *outcome = outcomes[view->ctx->action][failed];
    if(item->rule_native[0] != '\0')
        text_add(view->text, "%*s%s%s%s[%s]%s: %s\n", BLOCK_INDENT, "", view->style->bold,
                 view->style->severity, word, item->rule_native, view->style->reset, outcome);
    else
        text_add(view->text, "%*s%s%s%s%s: %s\n", BLOCK_INDENT, "", view->style->bold,
                 view->style->severity, word, view->style->reset, outcome);
}

/* A finding with nowhere to point at: the tool's own words, set off by the
   same rule a frame uses so the block still reads as one thing. A link that
   failed under a profile carrying no debug information is what this is for —
   the linker names the symbol, and nothing in anyone's source. */
static void write_quoted(view_state *view, const diagnostic *item) {
    write_message_beside_the_bar(view, item);
    write_bar(view);
}

/* A line no frame can hold: one the parser could not read at all. Printed as
   the tool wrote it, because a frame is a way of reading output and never a
   filter on it. */
static void write_passthrough(view_state *view, const diagnostic *item) {
    char message[DIAGNOSTIC_TEXT_MAX];
    strip_csi(item->message, message, sizeof message);
    text_add(view->text, "%*s%s\n", BLOCK_INDENT, "", message);
}

/* Past the frame limit: the normalized one-line form, which still says
   everything the frame would have said. */
static void write_overflow(view_state *view, const diagnostic *item) {
    char line[DIAGNOSTIC_TEXT_MAX + DIAGNOSTIC_PATH_MAX + 64];
    if(diagnostic_format(item, view->ctx->root, line, sizeof line))
        text_add(view->text, "%*s%s\n", BLOCK_INDENT, "", line);
}

static void write_one(view_state *view, const diagnostic *item) {
    if(is_enclosing_context(item))
        return;
    if(view->chain_count < CHAIN_MAX &&
       included_from(item, view->chain[view->chain_count], DIAGNOSTIC_PATH_MAX)) {
        view->chain_count++;
        return;
    }
    if(!opens_a_frame(item)) {
        /* A note explains the frame above it, so it continues that box rather
           than announcing itself as a finding of its own. One that landed
           nowhere, and anything the parser could not read, is passed through as
           the tool wrote it. */
        if(!has_location(item) || item->severity == diagnostic_severity_unknown)
            write_passthrough(view, item);
        else if(view->frames > 0 && view->frames <= FRAME_LIMIT)
            write_frame(view, item, RULE_TEE, "note: ");
        else
            write_overflow(view, item);
        return;
    }
    view->frames++;
    if(view->frames > FRAME_LIMIT) {
        write_overflow(view, item);
        return;
    }
    text_add(view->text, "\n");
    write_summary(view, item);
    text_add(view->text, "\n");
    if(has_location(item))
        write_frame(view, item, RULE_CORNER, "");
    else
        write_quoted(view, item);
}

/* --- the block --- */

/* What this unit is, and whether it survived. The glyph carries the verdict as
   much as the words do, which is what makes a wall of these scannable. */
static void write_header(view_state *view, bool failed) {
    static const char *const verbs[][2] = {
        [diagnostic_view_compiling] = {"Warnings compiling", "Failed to compile"},
        [diagnostic_view_linking] = {"Warnings linking", "Failed to link"},
    };
    const char *glyph = failed ? GLYPH_FAILED : GLYPH_WARNED;
    const char *verb = verbs[view->ctx->action][failed];
    const char *unit = view->ctx->unit != NULL ? view->ctx->unit : "this unit";
    text_add(view->text, "%*s%s%s%s %s%s `%s`\n", BLOCK_INDENT, "", view->style->bold,
             view->style->severity, glyph, verb, view->style->reset, unit);
}

static void write_footer_line(view_state *view, const char *key, const char *value) {
    if(value == NULL || value[0] == '\0')
        return;
    text_add(view->text, "%s%*s = %s:%s %s\n", view->style->dim, (int)view->gutter, "", key,
             view->style->reset, value);
}

/* Whose code this was and what was asked to build it. Every line is omitted
   when it has nothing to say: a project's own source belongs to no package,
   and a dependency fetched into the shared cache has no path worth printing —
   its coordinate on the line above already says where it came from. */
static void write_footer(view_state *view) {
    const diagnostic_context *ctx = view->ctx;
    if(ctx->package != NULL && ctx->package[0] != '\0') {
        char named[DIAGNOSTIC_PATH_MAX];
        if(ctx->version != NULL && ctx->version[0] != '\0')
            snprintf(named, sizeof named, "%s v%s", ctx->package, ctx->version);
        else
            snprintf(named, sizeof named, "%s", ctx->package);
        write_footer_line(view, "dependency", named);
    }
    write_footer_line(view, "source", ctx->source);
    write_footer_line(view, "compiler", ctx->compiler);
}

char *diagnostic_view_render(const diagnostic_list *list, const diagnostic_context *ctx,
                             bool colour) {
    if(list == NULL || ctx == NULL || list->count == 0)
        return NULL;

    bool *keep = keep_flags(list);
    if(keep == NULL)
        return NULL;
    if(!anything_to_draw(list, keep)) {
        free(keep);
        return NULL;
    }

    const bool failed = any_error(list, keep);
    const view_style style = style_for(colour, failed);
    view_text text;
    text_init(&text);
    view_state view = {
        .text = &text, .style = &style, .ctx = ctx, .gutter = gutter_width(list, keep)};

    write_header(&view, failed);
    for(size_t i = 0; i < list->count; i++) {
        if(keep[i] && list->items[i].message[0] != '\0')
            write_one(&view, &list->items[i]);
    }
    write_footer(&view);
    free(keep);

    if(text.failed || text.used == 0) {
        text_free(&text);
        return NULL;
    }
    return text.data;
}

void diagnostic_view_write(FILE *out, const diagnostic_list *list, const diagnostic_context *ctx,
                           bool colour) {
    char *block = diagnostic_view_render(list, ctx, colour);
    if(block == NULL)
        return;
    (void)fputs(block, out);
    free(block);
}
