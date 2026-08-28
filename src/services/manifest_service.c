#include <molto/services/manifest_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool manifest_is_valid_name(const char *name) {
    if(name == NULL || name[0] == '\0')
        return false;
    if(!(name[0] >= 'a' && name[0] <= 'z'))
        return false;
    for(size_t i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if(!allowed)
            return false;
    }
    return true;
}

/* The range operators. Two-character ones come first so that at a given
   position ">=" is reported as ">=" and not as ">". Whitespace is one of them:
   "1.0.0 2.0.0" is a conjunction with the comma left out. */
static const char *const RANGE_OPERATORS[] = {">=", "<=", "^", "~", ">", "<",
                                              "*",  ",",  "=", " ", "\t"};

static void clear_operator(char *out, size_t size) {
    if(out != NULL && size > 0)
        out[0] = '\0';
}

/* The range operator `version` carries, or NULL.

   Scanned left to right rather than operator by operator, so what is reported
   is the first thing wrong with the string: "1.0.0, <2.0.0" is a conjunction
   whose comma comes before its '<', and naming the '<' would point past the
   actual mistake. */
static const char *range_operator_in(const char *version) {
    for(const char *p = version; *p != '\0'; p++) {
        for(size_t i = 0; i < sizeof RANGE_OPERATORS / sizeof RANGE_OPERATORS[0]; i++) {
            const char *candidate = RANGE_OPERATORS[i];
            if(strncmp(p, candidate, strlen(candidate)) == 0)
                return candidate;
        }
    }
    return NULL;
}

/* One or more digits, ending at `stop`. */
static bool is_number(const char *text, const char *stop) {
    if(text == stop)
        return false;
    for(const char *p = text; p < stop; p++) {
        if(*p < '0' || *p > '9')
            return false;
    }
    return true;
}

/* MAJOR.MINOR.PATCH, with the optional -prerelease and +build tails left
   unexamined beyond being non-empty: semver's job here is to order releases and
   to catch a typo, not to gate resolution (RFC-0008). */
static bool is_semver(const char *version) {
    const char *tail = version + strcspn(version, "-+");
    if(*tail != '\0' && tail[1] == '\0')
        return false; /* a "-" or "+" introducing nothing */

    const char *major = version;
    const char *first_dot = memchr(major, '.', (size_t)(tail - major));
    if(first_dot == NULL)
        return false;
    const char *minor = first_dot + 1;
    const char *second_dot = memchr(minor, '.', (size_t)(tail - minor));
    if(second_dot == NULL)
        return false;

    return is_number(major, first_dot) && is_number(minor, second_dot) &&
           is_number(second_dot + 1, tail);
}

bool manifest_is_exact_version(const char *version, char *out_operator, size_t operator_size) {
    clear_operator(out_operator, operator_size);
    if(version == NULL || version[0] == '\0')
        return false;

    const char *found = range_operator_in(version);
    if(found != NULL) {
        if(out_operator != NULL && operator_size > 0)
            snprintf(out_operator, operator_size, "%s", found);
        return false;
    }
    return is_semver(version);
}

/* --- the licence expression --- */

/* The three operators an expression joins its identifiers with. */
static bool is_license_operator(const char *token, size_t length) {
    return (length == 3 && strncmp(token, "AND", length) == 0) ||
           (length == 2 && strncmp(token, "OR", length) == 0) ||
           (length == 4 && strncmp(token, "WITH", length) == 0);
}

/* An SPDX identifier: letters, digits, dots and hyphens, with an optional
   trailing '+' for "or later". */
static bool is_license_id(const char *token, size_t length) {
    if(length > 0 && token[length - 1] == '+')
        length--;
    if(length == 0)
        return false;

    for(size_t i = 0; i < length; i++) {
        char c = token[i];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '-';
        if(!allowed)
            return false;
    }
    return true;
}

/* Where the token starting at `text` ends. A parenthesis is a token on its own;
   anything else runs to the next space or parenthesis. */
static size_t token_length(const char *text) {
    if(*text == '(' || *text == ')')
        return 1;
    size_t length = 0;
    while(text[length] != '\0' && text[length] != ' ' && text[length] != '\t' &&
          text[length] != '(' && text[length] != ')')
        length++;
    return length;
}

bool manifest_is_valid_license(const char *expression) {
    if(expression == NULL)
        return false;

    /* An expression alternates operands and operators, so one bit of state says
       what may come next; parentheses only nest it. Everything this rejects is
       a shape no licence has: a dangling AND, an unclosed paren, two
       identifiers with nothing joining them. */
    bool expect_operand = true;
    int depth = 0;

    for(const char *p = expression; *p != '\0';) {
        if(*p == ' ' || *p == '\t') {
            p++;
            continue;
        }

        size_t length = token_length(p);
        if(*p == '(') {
            if(!expect_operand)
                return false;
            depth++;
        } else if(*p == ')') {
            if(expect_operand || depth == 0)
                return false;
            depth--;
        } else if(is_license_operator(p, length)) {
            if(expect_operand)
                return false;
            expect_operand = true;
        } else {
            if(!expect_operand || !is_license_id(p, length))
                return false;
            expect_operand = false;
        }
        p += length;
    }
    return !expect_operand && depth == 0;
}

/* --- the publishing metadata --- */

/* One optional string field.
 *
 * Read into more room than it can keep, so a value past the limit is reported
 * as too long rather than quietly stored as a prefix of itself. An absent key
 * leaves `out` alone; a key that is there and is not a string is an error,
 * because the two are indistinguishable to the reader and only one of them is
 * what the author meant. */
static bool read_about_string(doc_view doc, const char *table, const char *key, char *out,
                              size_t out_size, char *err, size_t err_size) {
    char value[MANIFEST_DESCRIPTION_MAX * 2];
    if(!doc_get_string(doc, table, key, value, sizeof value)) {
        if(doc_has_key(doc, table, key))
            return set_error(err, err_size, "[%s].%s must be a string", table, key);
        return true;
    }

    if(strlen(value) >= out_size)
        return set_error(err, err_size, "[%s].%s is longer than %zu characters", table, key,
                         out_size - 1);
    snprintf(out, out_size, "%s", value);
    return true;
}

bool manifest_read_about(doc_view doc, const char *table, manifest_about *out, char *err,
                         size_t err_size) {
    memset(out, 0, sizeof *out);

    if(!read_about_string(doc, table, "description", out->description, sizeof out->description, err,
                          err_size) ||
       !read_about_string(doc, table, "license", out->license, sizeof out->license, err,
                          err_size) ||
       !read_about_string(doc, table, "homepage", out->homepage, sizeof out->homepage, err,
                          err_size) ||
       !read_about_string(doc, table, "repository", out->repository, sizeof out->repository, err,
                          err_size))
        return false;

    /* Checked here rather than wherever the licence is eventually printed: a
       malformed expression is a mistake in the file, and the file is what the
       person who can fix it is looking at. */
    if(out->license[0] != '\0' && !manifest_is_valid_license(out->license))
        return set_error(err, err_size, "[%s].license '%s' is not an SPDX licence expression",
                         table, out->license);

    return doc_read_strings(doc, table, "authors", out->authors[0], MANIFEST_MAX_AUTHORS,
                            MANIFEST_AUTHOR_MAX, &out->author_count, err, err_size);
}

/* The starter manifest. `std` is declared rather than left to the compiler's
   default, which varies by toolchain and version, so the project picks its own
   language standard instead of inheriting whatever the machine happens to have.
   `include` is declared too: headers under include/ are the normal layout for a
   C project, and a key that ships commented out teaches nothing — the build
   fails on the first #include and the explanation is a line the user never
   read. Scaffolding creates the directory, so the manifest never points at
   something absent. The remaining [target] keys stay commented: they document
   what can be set without adding settings the project did not ask for — and so
   does the publishing metadata, which nothing needs until the package is
   published and which nobody can fill in on the author's behalf. */
static const char manifest_template[] =
    "[package]\n"
    "name = \"%s\"\n"
    "version = \"0.1.0\"\n"
    "# artifact = \"static\"   # executable (default) | static | shared\n"
    "# description = \"\"      # one line, for a catalogue\n"
    "# license = \"MIT\"       # an SPDX expression: MIT OR Apache-2.0\n"
    "# homepage = \"\"\n"
    "# repository = \"\"\n"
    "# authors = []\n"
    "\n"
    "[target]\n"
    "std = \"c17\"            # C standard passed as -std=\n"
    "include = [\"include\"]  # -Iinclude: the project's own headers\n"
    "# compiler = \"gcc\"     # gcc | clang | msvc; absent = autodetect\n"
    "# cpp_std = \"c++17\"    # C++ standard for C++ translation units\n"
    "# link = [\"m\"]         # system libraries: -lm\n"
    "# defines = []         # -D...\n"
    "# flags = []           # passed verbatim to the compiler and the linker\n"
    "\n"
    "[profile.debug]\n"
    "opt_level = 0\n"
    "debug_info = true\n"
    "\n"
    "[profile.release]\n"
    "opt_level = 3\n"
    "debug_info = false\n";

char *manifest_render_default(const char *name) {
    if(!manifest_is_valid_name(name))
        return NULL;
    int needed = snprintf(NULL, 0, manifest_template, name);
    if(needed < 0)
        return NULL;
    size_t size = (size_t)needed + 1;
    char *buffer = malloc(size);
    if(buffer == NULL)
        return NULL;
    snprintf(buffer, size, manifest_template, name);
    return buffer;
}
