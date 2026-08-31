#include <moltest.h>

#include <molto/util/glob.h>

#include <stdio.h>

/*
 * The table is not invented: every expectation below was read off glibc's own
 * `fnmatch` with no flags, which is what Molto used until mingw turned out to
 * ship no <fnmatch.h> at all. Written down rather than compared at run time
 * because the reference does not exist on the platform this port is for, and a
 * differential test that can only run on one side proves the wrong half.
 */
static const struct {
    const char *pattern;
    const char *text;
    bool matches;
} CASES[] = {
    {"*", "", true},
    {"*", "a/b/c", true},

    /* The one the exclusions actually rest on: no FNM_PATHNAME, so a star
       crosses a slash and a trailing globstar reaches all the way down. */
    {"build/**", "build/x/y.o", true},
    {"build/**", "build/", true},
    {"build/*", "build/x/y.o", true},
    {"vendor/**", "vendor/lib/x.c", true},

    /* And what it does not reach, which is why style_excludes_match tries the
       pattern a second time with the suffix removed. */
    {"vendor/**", "vendor", false},

    {"*.c", "main.c", true},
    {"*.c", "src/main.c", true},
    {"src/*.c", "src/main.c", true},

    {"?", "a", true},
    {"?", "ab", false},
    {"a?c", "a/c", true},

    {"[abc]x", "bx", true},
    {"[abc]x", "dx", false},
    {"[a-c]x", "cx", true},
    {"x[a-c]y", "xby", true},
    {"[!a-c]x", "dx", true},
    {"[!a-c]x", "ax", false},
    {"[^a]", "b", true},
    {"[]]", "]", true},
    {"[!]]", "a", true},

    {"[", "[", true},
    {"[abc", "[abc", true},

    /* Divergence, stated rather than hidden: glibc answers false here while
       answering true for `[abc` above, which are the same rule applied twice.
       Molto keeps the rule — an unterminated bracket is a literal bracket —
       because a matcher whose exceptions cannot be written down is a matcher
       nobody can predict. Nothing in a configuration file writes this. */
    {"a[b-", "a[b-", true},

    {"a\\*b", "a*b", true},
    {"a\\*b", "axb", false},

    {"**", "a/b", true},
    {"*/*", "a/b", true},
    {"*/*", "ab", false},
    {"a*b*c", "axxbyyc", true},
    {"a*b*c", "axxbyy", false},

    {"", "", true},
    {"", "a", false},
};

MOLTEST(glob_answers_what_fnmatch_answered) {
    for(size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const bool got = glob_match(CASES[i].pattern, CASES[i].text);

        /* Named rather than counted: a table of thirty cases that reports
           "assertion 17 failed" tells you nothing you can act on. */
        char what[160];
        snprintf(what, sizeof what, "\"%s\" against \"%s\" should %s", CASES[i].pattern,
                 CASES[i].text, CASES[i].matches ? "match" : "not match");
        if(got != CASES[i].matches)
            WARN(what);
        EXPECT_TRUE(got == CASES[i].matches);
    }
}

MOLTEST(glob_does_not_go_exponential_on_stars) {
    /* The recursive matcher this one replaces would still be running: eight
       stars against sixty characters that never match is 2^60 paths through
       it. Linear here, so the assertion is that it returns at all. */
    const char *pattern = "*a*a*a*a*a*a*a*b";
    const char *text = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    EXPECT_FALSE(glob_match(pattern, text));
}
