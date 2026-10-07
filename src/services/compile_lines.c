#include <molto/services/compile_lines.h>

#include <molto/services/fs_service.h>
#include <molto/util/json.h>
#include <molto/util/json_write.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINES_PATH_MAX 4096

static bool set_error(char *err, size_t err_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static bool set_error(char *err, size_t err_size, const char *format, ...) {
    if(err != NULL && err_size > 0) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(err, err_size, format, args);
        va_end(args);
    }
    return false;
}

void compile_lines_init(compile_lines *out) {
    out->lines = NULL;
    out->count = 0;
    out->capacity = 0;
}

void compile_lines_free(compile_lines *out) {
    for(size_t i = 0; i < out->count; i++) {
        free(out->lines[i].source);
        str_list_free(&out->lines[i].args);
    }
    free(out->lines);
    compile_lines_init(out);
}

static compile_line *add_line(compile_lines *out, const char *source) {
    if(out->count == out->capacity) {
        const size_t capacity = out->capacity == 0 ? 256 : out->capacity * 2;
        compile_line *grown = realloc(out->lines, capacity * sizeof *grown);
        if(grown == NULL)
            return NULL;
        out->lines = grown;
        out->capacity = capacity;
    }
    compile_line *line = &out->lines[out->count];
    line->source = strdup(source);
    if(line->source == NULL)
        return NULL;
    str_list_init(&line->args);
    out->count++;
    return line;
}

/* --- words --- */

bool compile_lines_split(const char *text, str_list *out) {
    const size_t length = strlen(text);
    char *word = malloc(length + 1);
    if(word == NULL)
        return false;
    size_t used = 0;
    bool in_word = false;
    bool ok = true;
    for(const char *c = text; ok && *c != '\0'; c++) {
        if(*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') {
            if(in_word) {
                word[used] = '\0';
                ok = str_list_push(out, word);
                used = 0;
                in_word = false;
            }
            continue;
        }
        in_word = true;
        if(*c == '\'') {
            for(c++; *c != '\0' && *c != '\''; c++)
                word[used++] = *c;
            if(*c == '\0')
                break;
        } else if(*c == '"') {
            for(c++; *c != '\0' && *c != '"'; c++) {
                /* Inside double quotes a backslash escapes only what would
                   otherwise mean something there. */
                if(*c == '\\' && (c[1] == '"' || c[1] == '\\' || c[1] == '$' || c[1] == '`'))
                    c++;
                word[used++] = *c;
            }
            if(*c == '\0')
                break;
        } else if(*c == '\\' && c[1] != '\0') {
            word[used++] = *++c;
        } else {
            word[used++] = *c;
        }
    }
    if(ok && in_word) {
        word[used] = '\0';
        ok = str_list_push(out, word);
    }
    free(word);
    return ok;
}

/* --- paths --- */

static void forward_slashes(char *path) {
    for(char *c = path; *c != '\0'; c++) {
        if(*c == '\\')
            *c = '/';
    }
}

/* `path` with `.` and `..` folded away lexically, in place. A `..` that would
   climb above the start is kept, so a path outside stays visibly outside. */
static void fold_dots(char *path) {
    const bool absolute = path[0] == '/';
    char *copy = strdup(path);
    if(copy == NULL)
        return;
    /* Where each kept segment starts in `copy`, which is cut at every slash. */
    char *segments[LINES_PATH_MAX / 2];
    size_t count = 0;
    for(char *part = copy; part != NULL;) {
        char *slash = strchr(part, '/');
        if(slash != NULL)
            *slash = '\0';
        if(part[0] == '\0' || strcmp(part, ".") == 0) {
            /* nothing */
        } else if(strcmp(part, "..") == 0 && count > 0 && strcmp(segments[count - 1], "..") != 0) {
            count--;
        } else if(count < sizeof segments / sizeof segments[0]) {
            segments[count++] = part;
        }
        part = slash != NULL ? slash + 1 : NULL;
    }
    size_t used = 0;
    if(absolute)
        path[used++] = '/';
    for(size_t i = 0; i < count; i++)
        used += (size_t)sprintf(path + used, "%s%s", i == 0 ? "" : "/", segments[i]);
    path[used] = '\0';
    if(used == 0)
        strcpy(path, ".");
    free(copy);
}

/* `path`, read in `cwd`, as an absolute path with forward slashes. */
static bool anchor(const char *cwd, const char *path, char *out, size_t size) {
    char raw[LINES_PATH_MAX];
    snprintf(raw, sizeof raw, "%s", path);
    forward_slashes(raw);
    const int written = fs_path_is_absolute(raw) ? snprintf(out, size, "%s", raw)
                                                 : snprintf(out, size, "%s/%s", cwd, raw);
    if(written < 0 || (size_t)written >= size)
        return false;
    /* A drive letter is not a segment fold_dots should touch. */
    char *start = (out[0] != '\0' && out[1] == ':') ? out + 2 : out;
    fold_dots(start);
    return true;
}

/* `absolute` relative to `root`, or false when it is not inside it. */
static bool inside(const char *root, const char *absolute, char *out, size_t size) {
    const size_t length = strlen(root);
    if(strncmp(absolute, root, length) != 0 || absolute[length] != '/')
        return false;
    const char *rest = absolute + length + 1;
    if(rest[0] == '\0' || strncmp(rest, "../", 3) == 0)
        return false;
    snprintf(out, size, "%s", rest);
    return true;
}

/* --- reading one line --- */

static bool has_suffix(const char *text, const char *suffix) {
    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length && strcmp(text + text_length - suffix_length, suffix) == 0;
}

static bool is_source(const char *word) {
    static const char *const SUFFIXES[] = {".c", ".cc", ".cpp", ".m", ".S", ".s", ".asm"};
    for(size_t i = 0; i < sizeof SUFFIXES / sizeof SUFFIXES[0]; i++) {
        if(has_suffix(word, SUFFIXES[i]))
            return true;
    }
    return false;
}

static bool same_program(const char *word, const char *driver) {
    if(driver == NULL || driver[0] == '\0')
        return false;
    char a[LINES_PATH_MAX];
    char b[LINES_PATH_MAX];
    snprintf(a, sizeof a, "%s", word);
    snprintf(b, sizeof b, "%s", driver);
    forward_slashes(a);
    forward_slashes(b);
    return strcmp(a, b) == 0;
}

/* Options followed by a path, joined (`-I.`) or not (`-I .`), which have to be
   anchored: a compiler's, and NASM's, whose `-P` preincludes a file (where a
   compiler's `-P` is a flag of its own). Longest first, so `-isystem` is never
   read as `-i` and a path. */
static const char *const CC_PATH_OPTIONS[] = {"-idirafter", "-isystem", "-include",
                                              "-imacros",   "-iquote",  "-I"};
static const char *const NASM_PATH_OPTIONS[] = {"-I", "-P"};

/* Options molto chooses for itself, and so drops; the first group takes the
   word after it. For NASM `-MD` names its file too. */
static const char *const DROP_WITH_VALUE[] = {"-o", "-MF", "-MT", "-MQ"};
static const char *const DROP_ALONE[] = {"-c", "-MMD", "-MD", "-MP", "-M", "-MM", "-MG"};

static bool listed(const char *word, const char *const list[], size_t count) {
    for(size_t i = 0; i < count; i++) {
        if(strcmp(word, list[i]) == 0)
            return true;
    }
    return false;
}

/* One command: the driver first, then what it was given. Not a compile line
   is not an error, only nothing added. */
static bool read_command(const str_list *words, const char *cwd, const char *root,
                         const compile_drivers *drivers, compile_lines *out, char *err,
                         size_t err_size) {
    const size_t count = str_list_count(words);
    if(count < 2)
        return true;
    const char *program = str_list_get(words, 0);
    const bool nasm = drivers->nasm != NULL && same_program(program, drivers->nasm);
    if(!nasm && !same_program(program, drivers->cc) && !same_program(program, drivers->cxx))
        return true;
    const char *const *path_options = nasm ? NASM_PATH_OPTIONS : CC_PATH_OPTIONS;
    const size_t path_option_count = nasm ? sizeof NASM_PATH_OPTIONS / sizeof NASM_PATH_OPTIONS[0]
                                          : sizeof CC_PATH_OPTIONS / sizeof CC_PATH_OPTIONS[0];

    /* The input: the last word that names a source and is nobody's value. */
    size_t input = 0;
    bool compiles = nasm;
    for(size_t i = 1; i < count; i++) {
        const char *word = str_list_get(words, i);
        if(strcmp(word, "-c") == 0)
            compiles = true;
        if(listed(word, DROP_WITH_VALUE, sizeof DROP_WITH_VALUE / sizeof DROP_WITH_VALUE[0]) ||
           (nasm && strcmp(word, "-MD") == 0) || listed(word, path_options, path_option_count)) {
            i++;
            continue;
        }
        if(word[0] != '-' && is_source(word))
            input = i;
    }
    if(!compiles || input == 0)
        return true;

    char absolute[LINES_PATH_MAX];
    char relative[LINES_PATH_MAX];
    if(!anchor(cwd, str_list_get(words, input), absolute, sizeof absolute) ||
       !inside(root, absolute, relative, sizeof relative))
        return set_error(err, err_size, "upstream's build compiles '%s', which is not inside %s",
                         str_list_get(words, input), root);

    compile_line *line = add_line(out, relative);
    if(line == NULL)
        return set_error(err, err_size, "out of memory reading upstream's compile lines");
    for(size_t i = 1; i < count; i++) {
        const char *word = str_list_get(words, i);
        if(i == input)
            continue;
        /* Before the flags that stand alone: NASM's `-MD` names its file. */
        if(listed(word, DROP_WITH_VALUE, sizeof DROP_WITH_VALUE / sizeof DROP_WITH_VALUE[0]) ||
           (nasm && strcmp(word, "-MD") == 0)) {
            i++;
            continue;
        }
        if(listed(word, DROP_ALONE, sizeof DROP_ALONE / sizeof DROP_ALONE[0]))
            continue;
        char anchored[LINES_PATH_MAX + 16];
        const char *kept = word;
        for(size_t p = 0; p < path_option_count; p++) {
            const char *option = path_options[p];
            const size_t length = strlen(option);
            if(strncmp(word, option, length) != 0)
                continue;
            const bool joined = word[length] != '\0';
            if(!joined && i + 1 >= count)
                break;
            const char *value = joined ? word + length : str_list_get(words, ++i);
            char path[LINES_PATH_MAX];
            if(!anchor(cwd, value, path, sizeof path))
                return set_error(err, err_size, "the path '%s' is too long", value);
            /* A preincluded file is looked for beside the line first and on
               the include path after: `-include pthread.h` names a system
               header, and anchoring it would name a file that is not there. */
            const bool file = strcmp(option, "-include") == 0 || strcmp(option, "-imacros") == 0 ||
                              strcmp(option, "-P") == 0;
            if(file && !fs_path_exists(path))
                snprintf(path, sizeof path, "%s", value);
            /* NASM needs the slash a directory include was written with. */
            const size_t value_length = strlen(value);
            const char *slash = value_length > 0 && value[value_length - 1] == '/' ? "/" : "";
            if(joined) {
                snprintf(anchored, sizeof anchored, "%s%s%s", option, path, slash);
            } else {
                if(!str_list_push(&line->args, option))
                    return set_error(err, err_size, "out of memory reading compile lines");
                snprintf(anchored, sizeof anchored, "%s%s", path, slash);
            }
            kept = anchored;
            break;
        }
        if(!str_list_push(&line->args, kept))
            return set_error(err, err_size, "out of memory reading upstream's compile lines");
    }
    return true;
}

/* A shell line split at `;`, `&&` and `||` outside quotes, each part read as
   a command of its own: make prints `printf …; gcc …` as one line. */
static bool read_shell_line(const char *text, const char *cwd, const char *root,
                            const compile_drivers *drivers, compile_lines *out, char *err,
                            size_t err_size) {
    char *copy = strdup(text);
    if(copy == NULL)
        return set_error(err, err_size, "out of memory reading upstream's compile lines");
    bool ok = true;
    char quote = '\0';
    char *start = copy;
    for(char *c = copy;; c++) {
        const bool end = *c == '\0';
        if(!end && quote != '\0') {
            if(*c == quote)
                quote = '\0';
            else if(*c == '\\' && quote == '"' && c[1] != '\0')
                c++;
            continue;
        }
        if(!end && (*c == '\'' || *c == '"')) {
            quote = *c;
            continue;
        }
        if(!end && *c == '\\' && c[1] != '\0') {
            c++;
            continue;
        }
        const bool separator =
            end || *c == ';' || (*c == '&' && c[1] == '&') || (*c == '|' && c[1] == '|');
        if(!separator)
            continue;
        const bool two = !end && *c != ';';
        *c = '\0';
        str_list words;
        str_list_init(&words);
        if(!compile_lines_split(start, &words))
            ok = set_error(err, err_size, "out of memory reading upstream's compile lines");
        else
            ok = read_command(&words, cwd, root, drivers, out, err, err_size);
        str_list_free(&words);
        if(end || !ok)
            break;
        if(two)
            c++;
        start = c + 1;
    }
    free(copy);
    return ok;
}

/* `make[1]: Entering directory '/x/y'`, with either quote make has used. */
static bool entering(const char *line, char *out, size_t size) {
    const char *at = strstr(line, ": Entering directory ");
    if(at == NULL || strncmp(line, "make", 4) != 0)
        return false;
    at += strlen(": Entering directory ");
    if(*at == '\'' || *at == '`' || *at == '"')
        at++;
    snprintf(out, size, "%s", at);
    size_t length = strlen(out);
    while(length > 0 && (out[length - 1] == '\'' || out[length - 1] == '"' ||
                         out[length - 1] == '\r' || out[length - 1] == '\n'))
        out[--length] = '\0';
    forward_slashes(out);
    return true;
}

bool compile_lines_from_make(const char *output, const char *root, const compile_drivers *drivers,
                             compile_lines *out, char *err, size_t err_size) {
    char base[LINES_PATH_MAX];
    snprintf(base, sizeof base, "%s", root);
    forward_slashes(base);

    /* The directories a recursive make has entered, innermost last. */
    char stack[16][LINES_PATH_MAX];
    size_t depth = 0;
    snprintf(stack[0], sizeof stack[0], "%s", base);

    char *copy = strdup(output);
    if(copy == NULL)
        return set_error(err, err_size, "out of memory reading upstream's compile lines");
    bool ok = true;
    /* A trailing backslash continues a line, as make prints a long recipe. */
    for(char *c = copy; *c != '\0'; c++) {
        if(c[0] == '\\' && c[1] == '\n') {
            c[0] = ' ';
            c[1] = ' ';
        }
    }
    for(char *line = copy; ok && line != NULL;) {
        char *newline = strchr(line, '\n');
        if(newline != NULL)
            *newline = '\0';
        char *next = newline != NULL ? newline + 1 : NULL;
        char dir[LINES_PATH_MAX];
        if(entering(line, dir, sizeof dir)) {
            if(depth + 1 < sizeof stack / sizeof stack[0])
                snprintf(stack[++depth], sizeof stack[0], "%s", dir);
        } else if(strncmp(line, "make", 4) == 0 && strstr(line, ": Leaving directory ") != NULL) {
            if(depth > 0)
                depth--;
        } else {
            ok = read_shell_line(line, stack[depth], base, drivers, out, err, err_size);
        }
        line = next;
    }
    free(copy);
    return ok;
}

bool compile_lines_from_database(const char *json, const char *root, const compile_drivers *drivers,
                                 compile_lines *out, char *err, size_t err_size) {
    json_document *doc = json_parse(json);
    if(doc == NULL)
        return set_error(err, err_size, "compile_commands.json is not valid JSON");
    char base[LINES_PATH_MAX];
    snprintf(base, sizeof base, "%s", root);
    forward_slashes(base);

    const json_value entries = json_root(doc);
    bool ok = json_type_of(entries) == json_type_array ||
              set_error(err, err_size, "compile_commands.json is not an array");
    for(size_t i = 0; ok && i < json_count(entries); i++) {
        const json_value entry = json_at(entries, i);
        const char *directory = json_string(json_get(entry, "directory"));
        char cwd[LINES_PATH_MAX];
        snprintf(cwd, sizeof cwd, "%s", directory != NULL ? directory : base);
        forward_slashes(cwd);

        str_list words;
        str_list_init(&words);
        const json_value arguments = json_get(entry, "arguments");
        const char *command = json_string(json_get(entry, "command"));
        if(json_type_of(arguments) == json_type_array) {
            for(size_t a = 0; ok && a < json_count(arguments); a++) {
                const char *word = json_string(json_at(arguments, a));
                ok = word != NULL && str_list_push(&words, word);
            }
            if(!ok)
                set_error(err, err_size,
                          "compile_commands.json entry %zu has an argument that "
                          "is not a string",
                          i + 1);
        } else if(command != NULL) {
            ok = compile_lines_split(command, &words) ||
                 set_error(err, err_size, "out of memory reading compile_commands.json");
        }
        if(ok)
            ok = read_command(&words, cwd, base, drivers, out, err, err_size);
        str_list_free(&words);
    }
    json_free(doc);
    return ok;
}

/* --- kept beside the configuration --- */

bool compile_lines_write(const compile_lines *lines, const char *path) {
    char temporary[LINES_PATH_MAX];
    if(!fs_format_path(temporary, sizeof temporary, "%s.tmp", path))
        return false;
    FILE *stream = fopen(temporary, "wb");
    if(stream == NULL)
        return false;
    json_writer writer;
    json_writer_init(&writer, stream);
    json_array_open(&writer, NULL);
    for(size_t i = 0; i < lines->count; i++) {
        json_object_open(&writer, NULL);
        json_write_field(&writer, "file", lines->lines[i].source);
        json_array_open(&writer, "arguments");
        for(size_t a = 0; a < str_list_count(&lines->lines[i].args); a++)
            json_write_element(&writer, str_list_get(&lines->lines[i].args, a));
        json_array_close(&writer);
        json_object_close(&writer);
    }
    json_array_close(&writer);
    json_writer_finish(&writer);
    const bool written = ferror(stream) == 0;
    if(fclose(stream) != 0 || !written) {
        remove(temporary);
        return false;
    }
    return fs_replace(temporary, path);
}

bool compile_lines_read(const char *path, compile_lines *out) {
    char *text = fs_read_file(path);
    if(text == NULL)
        return false;
    json_document *doc = json_parse(text);
    free(text);
    if(doc == NULL)
        return false;
    const json_value entries = json_root(doc);
    bool ok = json_type_of(entries) == json_type_array;
    for(size_t i = 0; ok && i < json_count(entries); i++) {
        const json_value entry = json_at(entries, i);
        const char *file = json_string(json_get(entry, "file"));
        const json_value arguments = json_get(entry, "arguments");
        compile_line *line = file != NULL ? add_line(out, file) : NULL;
        ok = line != NULL && json_type_of(arguments) == json_type_array;
        for(size_t a = 0; ok && a < json_count(arguments); a++) {
            const char *word = json_string(json_at(arguments, a));
            ok = word != NULL && str_list_push(&line->args, word);
        }
    }
    json_free(doc);
    if(!ok)
        compile_lines_free(out);
    return ok;
}
