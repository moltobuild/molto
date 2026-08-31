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

bool depfile_parse(const char *text, str_list *out) {
    const char *colon = strchr(text, ':');
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
