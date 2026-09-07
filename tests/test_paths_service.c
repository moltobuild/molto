#include <moltest.h>

#include "private_home.h"

#include <molto/services/paths_service.h>

#include <stdlib.h>
#include <string.h>

/*
 * Where molto keeps its own things.
 *
 * One function answers this for the four places that used to read `HOME`
 * apiece, and the order it asks in is the whole contract: `$MOLTO_HOME` wins,
 * then `$HOME`, then — on Windows — `%USERPROFILE%`, because cmd and
 * PowerShell set that one and leave `HOME` unset. Getting the order wrong is
 * not a crash; it is a credential written somewhere the next command does not
 * look, which is why it is pinned here rather than left to the four callers.
 */

typedef struct {
    char home[PRIVATE_HOME_MAX];
    bool home_was_set;
    molto_home_override override;
#ifdef _WIN32
    char user_profile[PRIVATE_HOME_MAX];
    bool user_profile_was_set;
#endif
} environment;

static void remember(const char *name, char *out, size_t size, bool *was_set) {
    const char *value = getenv(name);
    *was_set = value != NULL;
    snprintf(out, size, "%s", value != NULL ? value : "");
}

static void put_back(const char *name, const char *value, bool was_set) {
    if(was_set)
        (void)setenv(name, value, 1);
    else
        (void)unsetenv(name);
}

static void save(environment *saved) {
    remember("HOME", saved->home, sizeof saved->home, &saved->home_was_set);
    molto_home_override_clear(&saved->override);
#ifdef _WIN32
    remember("USERPROFILE", saved->user_profile, sizeof saved->user_profile,
             &saved->user_profile_was_set);
#endif
}

static void restore(const environment *saved) {
    put_back("HOME", saved->home, saved->home_was_set);
    molto_home_override_restore(&saved->override);
#ifdef _WIN32
    put_back("USERPROFILE", saved->user_profile, saved->user_profile_was_set);
#endif
}

MOLTEST(the_molto_home_hangs_off_home) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/somewhere", 1));

    char home[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_TRUE(paths_molto_home(home, sizeof home));
    EXPECT_STREQ("/somewhere/.molto", home);

    restore(&saved);
}

/* The override the cache already had, given to the home as well. It answers
   whole rather than having `.molto` appended: someone who names a directory
   means that directory. */
MOLTEST(molto_home_overrides_home) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/somewhere", 1));
    ASSERT_EQ(0, setenv("MOLTO_HOME", "/elsewhere/molto", 1));

    char home[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_TRUE(paths_molto_home(home, sizeof home));
    EXPECT_STREQ("/elsewhere/molto", home);

    restore(&saved);
}

/* An override set to nothing is not an override. Without this an exported but
   empty variable would answer the empty path, and every molto file would be
   written at the root of the current drive. */
MOLTEST(an_empty_override_is_no_override) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/somewhere", 1));
    ASSERT_EQ(0, setenv("MOLTO_HOME", "", 1));

    char home[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_TRUE(paths_molto_home(home, sizeof home));
    EXPECT_STREQ("/somewhere/.molto", home);

    restore(&saved);
}

MOLTEST(a_subdirectory_hangs_off_the_home) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("MOLTO_HOME", "/elsewhere/molto", 1));

    char path[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_TRUE(paths_molto_subdir("plugins/bin", path, sizeof path));
    EXPECT_STREQ("/elsewhere/molto/plugins/bin", path);

    restore(&saved);
}

#ifdef _WIN32
/*
 * The failure this whole function was extracted for: `molto login` on an
 * ordinary Windows machine said "HOME is not set, so there is nowhere to store
 * credentials". cmd and PowerShell set USERPROFILE and leave HOME alone; only
 * MSYS2 and git-bash define the other one.
 */
MOLTEST(windows_falls_back_to_the_user_profile) {
    environment saved;
    save(&saved);

    (void)unsetenv("HOME");
    ASSERT_EQ(0, setenv("USERPROFILE", "C:/Users/somebody", 1));

    char home[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_TRUE(paths_molto_home(home, sizeof home));
    EXPECT_STREQ("C:/Users/somebody/.molto", home);

    restore(&saved);
}

/* HOME still wins where a shell went to the trouble of setting it: someone in
   MSYS2 means the home that shell gave them, and molto should agree with
   everything else run from there. */
MOLTEST(home_beats_the_user_profile_when_both_are_set) {
    environment saved;
    save(&saved);

    ASSERT_EQ(0, setenv("HOME", "/c/Users/somebody", 1));
    ASSERT_EQ(0, setenv("USERPROFILE", "D:/elsewhere", 1));

    char home[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_TRUE(paths_molto_home(home, sizeof home));
    EXPECT_STREQ("/c/Users/somebody/.molto", home);

    restore(&saved);
}
#else
/* Nothing to fall back to, and saying so is the point: a Unix with no HOME has
   no home, and inventing one would put a credential somewhere nobody looks. */
MOLTEST(no_home_is_no_answer) {
    environment saved;
    save(&saved);

    (void)unsetenv("HOME");

    char home[MOLTO_HOME_PATH_MAX] = "";
    EXPECT_FALSE(paths_molto_home(home, sizeof home));

    restore(&saved);
}
#endif
