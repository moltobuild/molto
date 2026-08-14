#include <molto/util/semver.h>

#include <molto/util/str_list.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* --- parsing --- */

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_identifier_char(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

/* One numeric component, up to the next `.` or the end of the numbers.
 *
 * Leading zeroes are refused because semver refuses them, and because
 * accepting them would make `1.01.0` and `1.1.0` two spellings of one version
 * that sort as two. */
static bool read_number(const char **at, unsigned long *out) {
    const char *p = *at;
    if(!is_digit(*p))
        return false;
    if(*p == '0' && is_digit(p[1]))
        return false;

    unsigned long value = 0;
    while(is_digit(*p)) {
        const unsigned long digit = (unsigned long)(*p - '0');
        if(value > (~0UL - digit) / 10)
            return false; /* a version number nobody wrote */
        value = value * 10 + digit;
        p++;
    }
    *at = p;
    *out = value;
    return true;
}

/* A dot-separated run of identifiers, as a pre-release or a build tag. Each
   identifier must be non-empty and alphanumeric-or-hyphen. */
static bool read_tag(const char **at, char *out, size_t out_size) {
    const char *p = *at;
    const char *start = p;
    size_t identifier = 0;

    for(;; p++) {
        if(is_identifier_char(*p)) {
            identifier++;
            continue;
        }
        if(identifier == 0)
            return false; /* empty identifier: "1.0.0-" or "1.0.0-a..b" */
        if(*p != '.')
            break;
        identifier = 0;
    }

    const size_t length = (size_t)(p - start);
    if(length >= out_size)
        return false;
    memcpy(out, start, length);
    out[length] = '\0';
    *at = p;
    return true;
}

bool semver_parse(const char *text, semver *out) {
    if(text == NULL || out == NULL)
        return false;
    memset(out, 0, sizeof *out);

    const char *p = text;
    if(!read_number(&p, &out->major) || *p != '.')
        return false;
    p++;
    if(!read_number(&p, &out->minor) || *p != '.')
        return false;
    p++;
    if(!read_number(&p, &out->patch))
        return false;

    if(*p == '-') {
        p++;
        if(!read_tag(&p, out->prerelease, sizeof out->prerelease))
            return false;
    }
    if(*p == '+') {
        p++;
        if(!read_tag(&p, out->build, sizeof out->build))
            return false;
    }
    return *p == '\0';
}

/* --- ordering --- */

static int compare_numbers(unsigned long a, unsigned long b) {
    if(a < b)
        return -1;
    return a > b ? 1 : 0;
}

/* The next dot-separated identifier, and where the one after it starts. */
static const char *next_identifier(const char *at, size_t *length) {
    const char *dot = strchr(at, '.');
    *length = dot == NULL ? strlen(at) : (size_t)(dot - at);
    return dot == NULL ? at + *length : dot + 1;
}

static bool all_digits(const char *at, size_t length) {
    for(size_t i = 0; i < length; i++) {
        if(!is_digit(at[i]))
            return false;
    }
    return length > 0;
}

/* Numeric identifiers compare as numbers and always precede alphanumeric ones;
   alphanumeric ones compare as text. */
static int compare_identifier(const char *a, size_t a_length, const char *b, size_t b_length) {
    const bool a_numeric = all_digits(a, a_length);
    const bool b_numeric = all_digits(b, b_length);
    if(a_numeric != b_numeric)
        return a_numeric ? -1 : 1;

    if(a_numeric) {
        /* Both are digit runs with no leading zeroes, so the longer is larger
           and equal lengths compare lexically — no conversion needed, and no
           overflow to worry about. */
        if(a_length != b_length)
            return a_length < b_length ? -1 : 1;
    }

    const size_t shared = a_length < b_length ? a_length : b_length;
    const int order = memcmp(a, b, shared);
    if(order != 0)
        return order < 0 ? -1 : 1;
    if(a_length == b_length)
        return 0;
    return a_length < b_length ? -1 : 1;
}

/* A version with a pre-release precedes the same version without one; between
   two pre-releases, identifiers decide, and more identifiers wins a tie. */
static int compare_prerelease(const char *a, const char *b) {
    if(a[0] == '\0' || b[0] == '\0') {
        if(a[0] == b[0])
            return 0;
        return a[0] == '\0' ? 1 : -1;
    }

    while(a[0] != '\0' && b[0] != '\0') {
        size_t a_length = 0;
        size_t b_length = 0;
        const char *a_next = next_identifier(a, &a_length);
        const char *b_next = next_identifier(b, &b_length);

        const int order = compare_identifier(a, a_length, b, b_length);
        if(order != 0)
            return order;
        a = a_next;
        b = b_next;
    }

    if(a[0] == b[0])
        return 0;
    return a[0] == '\0' ? -1 : 1;
}

int semver_compare(const semver *a, const semver *b) {
    int order = compare_numbers(a->major, b->major);
    if(order == 0)
        order = compare_numbers(a->minor, b->minor);
    if(order == 0)
        order = compare_numbers(a->patch, b->patch);
    if(order == 0)
        order = compare_prerelease(a->prerelease, b->prerelease);
    /* Build metadata takes no part: semver says two versions differing only
       there have the same precedence. */
    return order;
}

bool semver_highest(const char *const *versions, size_t count, char *out, size_t out_size) {
    const char *best_text = NULL;
    semver best;

    for(size_t i = 0; i < count; i++) {
        semver candidate;
        if(!semver_parse(versions[i], &candidate))
            continue;
        if(best_text == NULL || semver_compare(&candidate, &best) > 0) {
            best = candidate;
            best_text = versions[i];
        }
    }

    if(best_text == NULL)
        return false;
    return snprintf(out, out_size, "%s", best_text) > 0 && strlen(best_text) < out_size;
}

/* --- ordering a whole list --- */

/* Newest first among versions, and everything that is not a version after all
   of them. Two unparseable entries compare equal, which keeps qsort from
   reordering them against each other. */
static int compare_newest_first(const void *left, const void *right) {
    const char *a_text = *(const char *const *)left;
    const char *b_text = *(const char *const *)right;
    semver a;
    semver b;
    const bool a_is_version = semver_parse(a_text, &a);
    const bool b_is_version = semver_parse(b_text, &b);

    if(!a_is_version || !b_is_version)
        return (a_is_version ? 0 : 1) - (b_is_version ? 0 : 1);
    return -semver_compare(&a, &b);
}

size_t semver_sort_desc(str_list *list) {
    if(list->count > 1)
        qsort((void *)list->items, list->count, sizeof(char *), compare_newest_first);

    size_t versions = 0;
    while(versions < list->count) {
        semver parsed;
        if(!semver_parse(list->items[versions], &parsed))
            break;
        versions++;
    }
    return versions;
}
