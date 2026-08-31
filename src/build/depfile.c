#include <molto/build/depfile.h>

#include <molto/services/fs_service.h>

#include <stdlib.h>
#include <string.h>

/* Size of the buffer holding one prerequisite path while it is assembled. */
#define DEPFILE_TOKEN_MAX 4096

/* Flush the current token (if any) into the list and reset it. */
static bool flush_token(str_list *out, char *token, size_t *length) {
    if(*length == 0)
        return true;
    token[*length] = '\0';
    *length = 0;
    /* The compiler wrote this path, not Molto, so it is spelled the way that
       compiler spells one. Everything downstream compares it against paths
       Molto composed. */
    fs_to_one_separator(token);
    return str_list_push(out, token);
}

/* Append one character to the current token, guarding against overflow. */
static bool append_char(char *token, size_t *length, char c) {
    if(*length + 1 >= DEPFILE_TOKEN_MAX)
        return false;
    token[(*length)++] = c;
    return true;
}

/*
 * Whether this colon belongs to a drive letter rather than separating the
 * target from its prerequisites.
 *
 * `D:/build/debug/obj/main.c.o: D:/src/main.c` has three colons and only the
 * second one divides it. Splitting on the first hands back a prerequisite list
 * beginning `/build/debug/obj/main.c.o`, which is a path to nothing — so every
 * prerequisite fails to stat and the whole unit is recorded as unrecordable.
 * The build still works; it just stops being incremental, quietly.
 *
 * The question goes to the path service so the rule is stated once. On POSIX
 * `fs_path_is_absolute("d:/")` is false, so this is always false and the
 * parsing is exactly what it was — which matters, because a one-letter target
 * called `a` is legal there and its colon is the separator.
 */
static bool colon_belongs_to_a_drive(const char *text, const char *colon) {
    if(colon != text + 1)
        return false;
    const char probe[] = {text[0], ':', '/', '\0'};
    return fs_path_is_absolute(probe);
}

bool depfile_parse(const char *text, str_list *out) {
    const char *colon = strchr(text, ':');
    while(colon != NULL && colon_belongs_to_a_drive(text, colon))
        colon = strchr(colon + 1, ':');
    if(colon == NULL)
        return false;

    char token[DEPFILE_TOKEN_MAX];
    size_t length = 0;
    for(const char *p = colon + 1; *p != '\0'; p++) {
        char c = *p;
        if(c == '\\') {
            char next = p[1];
            if(next == '\n' || next == '\r') {
                /* Line continuation: acts as a separator. */
                if(!flush_token(out, token, &length))
                    return false;
                p++; /* consume the newline character */
            } else if(next == ' ') {
                /* Escaped space: a literal space inside a path. */
                if(!append_char(token, &length, ' '))
                    return false;
                p++;
            } else if(next != '\0') {
                /* Other escape (e.g. "\#", "\$"): keep the next char literally. */
                if(!append_char(token, &length, next))
                    return false;
                p++;
            }
        } else if(c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if(!flush_token(out, token, &length))
                return false;
        } else {
            if(!append_char(token, &length, c))
                return false;
        }
    }
    return flush_token(out, token, &length);
}

bool depfile_read(const char *path, str_list *out) {
    char *text = fs_read_file(path);
    if(text == NULL)
        return false;
    bool ok = depfile_parse(text, out);
    free(text);
    return ok;
}
