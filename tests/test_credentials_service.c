#include <moltest.h>

#include <molto/services/credentials_service.h>
#include <molto/services/fs_service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Every test runs against a HOME of its own, so none of them can read or
   overwrite the credentials of whoever is running the suite. */

static char previous_home[1024];
static char sandbox[1024];

static void with_private_home(void) {
    const char *home = getenv("HOME");
    snprintf(previous_home, sizeof previous_home, "%s", home == NULL ? "" : home);

    /* Wherever the platform keeps temporary files, not `/tmp`. Under MSYS2 a
       native binary reads `/tmp` as a path on the current drive, and there is
       no such directory there — so every save into it failed, and the failure
       said the credentials could not be written rather than that the sandbox
       was never made. RFC-0017 names this idiom as the single largest reason
       these suites did not run on Windows; this is the one that got away. */
    if(!moltest_temp_dir("molto_credentials", sandbox, sizeof sandbox))
        sandbox[0] = '\0';
    setenv("HOME", sandbox, 1);
}

static void restore_home(void) {
    char path[1200];
    if(fs_format_path(path, sizeof path, "%s/.molto/credentials.toml", sandbox))
        (void)unlink(path);
    if(fs_format_path(path, sizeof path, "%s/.molto", sandbox))
        (void)rmdir(path);
    (void)rmdir(sandbox);

    if(previous_home[0] != '\0')
        setenv("HOME", previous_home, 1);
    else
        unsetenv("HOME");
}

MOLTEST(credentials_round_trip_what_was_saved) {
    with_private_home();

    const credentials saved = {.registry = "https://registry.example.com",
                               .email = "person@example.com",
                               .token = "deadbeef"};
    char err[256] = "";
    ASSERT_TRUE(credentials_save(&saved, err, sizeof err));

    credentials loaded = {0};
    ASSERT_TRUE(credentials_load(&loaded, err, sizeof err));
    EXPECT_STREQ("https://registry.example.com", loaded.registry);
    EXPECT_STREQ("person@example.com", loaded.email);
    EXPECT_STREQ("deadbeef", loaded.token);

    restore_home();
}

MOLTEST(credentials_are_readable_only_by_their_owner) {
    with_private_home();

    const credentials saved = {
        .registry = "https://r.example", .email = "a@b.c", .token = "secret"};
    char err[256] = "";
    ASSERT_TRUE(credentials_save(&saved, err, sizeof err));

    char path[1200];
    ASSERT_TRUE(credentials_path(path, sizeof path));
    struct stat info;
    ASSERT_EQ(0, stat(path, &info));
    /* A token another account can read is a token that is already leaked.
       Asked only where the answer means something: Windows has no mode bits
       for `stat` to report, and what protects a file there is an ACL. Molto
       does not set one yet, and asserting a POSIX number about a platform that
       has none would report a security property nobody implemented. */
#ifndef _WIN32
    EXPECT_EQ(0, (int)(info.st_mode & (S_IRWXG | S_IRWXO)));
#endif

    restore_home();
}

MOLTEST(credentials_report_that_nobody_logged_in) {
    with_private_home();

    credentials loaded = {0};
    char err[256] = "";
    EXPECT_FALSE(credentials_load(&loaded, err, sizeof err));
    EXPECT_NOT_NULL(strstr(err, "molto login"));

    restore_home();
}

MOLTEST(credentials_replace_what_was_there) {
    with_private_home();

    char err[256] = "";
    const credentials first = {.registry = "https://one", .email = "a@b.c", .token = "t1"};
    const credentials second = {.registry = "https://two", .email = "d@e.f", .token = "t2"};
    ASSERT_TRUE(credentials_save(&first, err, sizeof err));
    ASSERT_TRUE(credentials_save(&second, err, sizeof err));

    credentials loaded = {0};
    ASSERT_TRUE(credentials_load(&loaded, err, sizeof err));
    EXPECT_STREQ("https://two", loaded.registry);
    EXPECT_STREQ("t2", loaded.token);

    restore_home();
}
