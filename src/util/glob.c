#include <molto/util/glob.h>

#include <stddef.h>

/* Where a bracket expression ends, or NULL if it never does.

   The first character inside can be a literal ']' — `[]]` is the set holding a
   close bracket — and so can the one right after a negation, which is why the
   scan steps over one character before it starts looking. */
static const char *bracket_end(const char *open) {
    const char *p = open + 1;
    if(*p == '!' || *p == '^')
        p++;
    if(*p == ']')
        p++;
    while(*p != '\0' && *p != ']') {
        if(*p == '\\' && p[1] != '\0')
            p++;
        p++;
    }
    return *p == ']' ? p : NULL;
}

/* Whether `c` is in the set at `open`, which is known to be terminated. */
static bool bracket_holds(const char *open, const char *close, char c) {
    const char *p = open + 1;
    bool negated = false;
    if(*p == '!' || *p == '^') {
        negated = true;
        p++;
    }

    bool found = false;
    while(p < close) {
        char low = *p;
        if(low == '\\' && p + 1 < close)
            low = *++p;

        /* A '-' is a range only between two members; trailing, it is itself. */
        if(p + 2 < close && p[1] == '-') {
            const char *high_at = p + 2;
            char high = *high_at;
            if(high == '\\' && high_at + 1 < close)
                high = *++high_at;
            if((unsigned char)c >= (unsigned char)low && (unsigned char)c <= (unsigned char)high)
                found = true;
            p = high_at + 1;
            continue;
        }

        if(low == c)
            found = true;
        p++;
    }
    return found != negated;
}

/* Match one pattern item against one character, advancing `cursor` past the
   item when it matches. On a mismatch the cursor is left where it was; no
   caller reads it, because a mismatch always means backtracking to a star. */
static bool item_matches(const char **cursor, char c) {
    const char *p = *cursor;

    if(*p == '?') {
        *cursor = p + 1;
        return true;
    }

    if(*p == '[') {
        const char *close = bracket_end(p);
        if(close != NULL) {
            if(!bracket_holds(p, close, c))
                return false;
            *cursor = close + 1;
            return true;
        }
        /* Unterminated, so the bracket was never a set. Falls through and is
           compared as the literal character it is. */
    }

    if(*p == '\\' && p[1] != '\0')
        p++;
    if(*p != c || *p == '\0')
        return false;
    *cursor = p + 1;
    return true;
}

/*
 * Iterative rather than recursive, and deliberately.
 *
 * The obvious recursive matcher calls itself once per position a star could
 * end at, which on a pattern of several stars against a long path is
 * exponential — and the input here is a pattern out of a configuration file,
 * matched against every source in the project. This remembers the last star
 * and the text position that followed it, and on a mismatch resumes from one
 * character later. Linear in the text for every pattern anyone writes.
 */
bool glob_match(const char *pattern, const char *text) {
    const char *after_star = NULL; /* the pattern just past the last star */
    const char *resume = NULL;     /* the text position to retry from */
    const char *p = pattern;
    const char *t = text;

    while(*t != '\0') {
        if(*p == '*') {
            after_star = ++p;
            resume = t;
            continue;
        }
        if(item_matches(&p, *t)) {
            t++;
            continue;
        }
        if(after_star == NULL)
            return false;
        p = after_star;
        t = ++resume;
    }

    /* Text exhausted: what is left of the pattern has to be stars. */
    while(*p == '*')
        p++;
    return *p == '\0';
}
