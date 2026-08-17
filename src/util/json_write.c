#include <molto/util/json_write.h>

/* Two spaces per level. A document meant to be committed and diffed is read by
   people, and 80 columns of leading tabs is not read by anyone. */
#define INDENT_WIDTH 2

void json_writer_init(json_writer *writer, FILE *stream) {
    writer->stream = stream;
    writer->depth = 0;
    writer->pending_comma = false;
    writer->empty[0] = true;
}

void json_writer_finish(json_writer *writer) { fputc('\n', writer->stream); }

void json_write_string(FILE *stream, const char *text) {
    fputc('"', stream);
    for(const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        switch(*p) {
        case '"':
            fputs("\\\"", stream);
            break;
        case '\\':
            fputs("\\\\", stream);
            break;
        case '\n':
            fputs("\\n", stream);
            break;
        case '\r':
            fputs("\\r", stream);
            break;
        case '\t':
            fputs("\\t", stream);
            break;
        default:
            /* Everything below a space has to leave as an escape; a raw one is
               a document no parser will take. */
            if(*p < 0x20)
                fprintf(stream, "\\u%04x", *p);
            else
                fputc((int)*p, stream);
            break;
        }
    }
    fputc('"', stream);
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
    fputc('\n', writer->stream);
    for(int i = 0; i < writer->depth * INDENT_WIDTH; i++)
        fputc(' ', writer->stream);
}

/* Everything that comes before a value: the comma the previous one earned, the
   line it goes on, and the key when it has one. */
static void begin_value(json_writer *writer, const char *key) {
    if(writer->pending_comma)
        fputc(',', writer->stream);
    if(writer->depth > 0)
        write_indent(writer);
    mark_current_used(writer);

    if(key != NULL) {
        json_write_string(writer->stream, key);
        fputs(": ", writer->stream);
    }
}

static void open_container(json_writer *writer, const char *key, char bracket) {
    begin_value(writer, key);
    fputc(bracket, writer->stream);

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

    fputc(bracket, writer->stream);
    writer->pending_comma = true;
}

void json_object_open(json_writer *writer, const char *key) { open_container(writer, key, '{'); }

void json_object_close(json_writer *writer) { close_container(writer, '}'); }

void json_array_open(json_writer *writer, const char *key) { open_container(writer, key, '['); }

void json_array_close(json_writer *writer) { close_container(writer, ']'); }

void json_write_field(json_writer *writer, const char *key, const char *value) {
    begin_value(writer, key);
    json_write_string(writer->stream, value);
    writer->pending_comma = true;
}

void json_write_element(json_writer *writer, const char *value) {
    json_write_field(writer, NULL, value);
}

void json_write_raw(json_writer *writer, const char *key, const char *literal) {
    begin_value(writer, key);
    fputs(literal, writer->stream);
    writer->pending_comma = true;
}
