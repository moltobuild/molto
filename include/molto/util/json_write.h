#ifndef MOLTO_JSON_WRITE_H
#define MOLTO_JSON_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * Writing JSON to a stream, as the counterpart of the reader in json.h.
 *
 * Molto emits three JSON documents — the diagnostics of `molto lint --format
 * json`, `compile_commands.json`, and a bill of materials — and until this
 * existed, two of them carried their own byte-identical copy of the escaping
 * function. Two copies of one escaper is where a quote nobody escaped becomes
 * a document no reader can parse, in one of the three and not the others.
 *
 * The writer streams: there is no tree in memory, and nothing to free. What it
 * keeps is the little that cannot be derived from the call — how deep it is,
 * whether the next value needs a comma in front of it, and whether the
 * container it is in has anything in it yet. That last one is what lets an
 * empty array come out as `[]` on one line, which is the ordinary shape for a
 * package with no dependencies.
 *
 * It does not validate. Closing an object that was never opened, or writing a
 * field at the top level, produces the document that describes — the caller
 * knows the shape it is writing, and the tests read it back with json_parse to
 * prove it.
 */

/* As deep as a document may nest. The same limit the reader uses. */
#define JSON_WRITE_MAX_DEPTH 64

typedef struct {
    FILE *stream;
    int depth;
    /* Whether a comma has to precede whatever is written next. */
    bool pending_comma;
    /* Per level: nothing has been written into that container yet. */
    bool empty[JSON_WRITE_MAX_DEPTH];
} json_writer;

void json_writer_init(json_writer *writer, FILE *stream);

/* The newline that ends the document. Separate from closing the root object,
   because whether a file ends in one is a property of the file and not of the
   value it holds. */
void json_writer_finish(json_writer *writer);

/* Escape `text` into a JSON string literal, quotes included.
 *
 * Standalone and taking a stream rather than a writer, because it is what the
 * two hand-rolled emitters already needed and all they needed. */
void json_write_string(FILE *stream, const char *text);

/* Open a container. `key` names it inside an object; NULL is what an element
   of an array — or the document's own root — passes. */
void json_object_open(json_writer *writer, const char *key);
void json_object_close(json_writer *writer);
void json_array_open(json_writer *writer, const char *key);
void json_array_close(json_writer *writer);

/* One `"key": "value"` pair. Strings only: every scalar a bill of materials
   writes is one, and a number nobody needs is a formatting decision nobody has
   to make. */
void json_write_field(json_writer *writer, const char *key, const char *value);

/* One string element of an array. */
void json_write_element(json_writer *writer, const char *value);

/* One pair whose value is written verbatim, unquoted and unescaped: a number,
   a bool, a null. The caller is stating that `literal` is already JSON, which
   is why this is the one entry point that can produce a document the reader
   will not take. Everything a caller has that came from outside goes through
   json_write_field instead. */
void json_write_raw(json_writer *writer, const char *key, const char *literal);

#endif /* MOLTO_JSON_WRITE_H */
