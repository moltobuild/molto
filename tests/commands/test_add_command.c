#include <moltest.h>

#include <molto/commands/add_command.h>

/* The one question `molto add` asks before it does anything: is there a
 * registry to reach, or only a file to rewrite?
 *
 * It is pinned here because two things read the same answer — the request and
 * the spinner turning at it — and a spinner that disagreed with the request
 * would either animate nothing or leave a row on the screen over work that had
 * already finished. The animation itself is pinned in test_loader.c; what is
 * worth a test here is the condition, not the drawing.
 */

/* A bare name is the whole reason the spinner exists: the newest release is a
   question only the registry can answer. */
MOLTEST(a_name_with_no_version_asks_the_registry) {
    EXPECT_TRUE(add_command_asks_registry(NULL, NULL));
}

/* `molto add ""@` and an option parsed into an empty string are the same
   nothing as no version at all, and are not left to reach the manifest as a
   version of zero characters. */
MOLTEST(an_empty_version_is_no_version) {
    EXPECT_TRUE(add_command_asks_registry("", NULL));
}

/* `molto add sqlite@3.53.4` states the answer. Asking anyway would be asking a
   question whose reply is thrown away. */
MOLTEST(an_exact_version_asks_nobody) {
    EXPECT_FALSE(add_command_asks_registry("3.53.4", NULL));
}

/* A path, a git URL or an archive carries its own bytes: there is no
   coordinate for a registry to look up, so nothing waits and nothing turns. */
MOLTEST(a_source_dependency_asks_nobody) {
    EXPECT_FALSE(add_command_asks_registry(NULL, "modules/http"));
}

/* A version beside a source is a git tag rather than a release (RFC-0002), so
   it does not turn the invocation into a registry question either. */
MOLTEST(a_tag_beside_a_source_asks_nobody) {
    EXPECT_FALSE(add_command_asks_registry("v1.2.0", "https://github.com/x/y.git"));
}
