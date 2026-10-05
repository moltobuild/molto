#include <moltest.h>

#include <molto/services/conflict_prompt.h>

#include <stdio.h>
#include <string.h>

/* The message RFC-0008 fixes, and the question it ends with. Rendering is
   tested away from the terminal on purpose: the text is the part that can be
   wrong, and it needs no tty to be wrong in. */

static dep_conflict a_conflict(void) {
    dep_conflict conflict = {0};
    snprintf(conflict.name, sizeof conflict.name, "png");
    snprintf(conflict.version, sizeof conflict.version, "1.6.40");
    snprintf(conflict.required_by, sizeof conflict.required_by, "libspng");
    snprintf(conflict.other_version, sizeof conflict.other_version, "1.5.30");
    snprintf(conflict.other_required_by, sizeof conflict.other_required_by, "cairo");
    return conflict;
}

static dep_conflict with_proposal(void) {
    dep_conflict conflict = a_conflict();
    conflict.has_proposal = true;
    snprintf(conflict.change_name, sizeof conflict.change_name, "cairo");
    snprintf(conflict.change_from, sizeof conflict.change_from, "1.18.0");
    snprintf(conflict.change_to, sizeof conflict.change_to, "1.18.2");
    snprintf(conflict.change_table, sizeof conflict.change_table, "deps");
    snprintf(conflict.settles_on, sizeof conflict.settles_on, "1.6.40");
    return conflict;
}

MOLTEST(the_message_names_both_versions_and_who_asked_for_each) {
    const dep_conflict conflict = a_conflict();
    char text[1024] = "";

    conflict_prompt_render(&conflict, text, sizeof text);

    EXPECT_NOT_NULL(strstr(text, "png is required at two versions"));
    EXPECT_NOT_NULL(strstr(text, "1.6.40"));
    EXPECT_NOT_NULL(strstr(text, "required by libspng"));
    EXPECT_NOT_NULL(strstr(text, "1.5.30"));
    EXPECT_NOT_NULL(strstr(text, "required by cairo"));
}

/* The root package is named by its relationship to the reader: they are the
   one being asked, and "required by molto" would tell them nothing. */
MOLTEST(a_direct_dependency_is_attributed_to_this_project) {
    dep_conflict conflict = a_conflict();
    conflict.other_required_by[0] = '\0';
    char text[1024] = "";

    conflict_prompt_render(&conflict, text, sizeof text);

    EXPECT_NOT_NULL(strstr(text, "required by this project"));
}

MOLTEST(a_proposal_says_what_to_change_and_what_it_settles_on) {
    const dep_conflict conflict = with_proposal();
    char text[1024] = "";

    conflict_prompt_render(&conflict, text, sizeof text);

    EXPECT_NOT_NULL(strstr(text, "Upgrading cairo to 1.18.2 requires png 1.6.40"));
}

/* Finding nothing is not a dead end: the user still declared two versions and
   can change either, and the message has to say so. */
MOLTEST(without_a_proposal_the_message_still_leaves_an_action) {
    const dep_conflict conflict = a_conflict();
    char text[1024] = "";

    conflict_prompt_render(&conflict, text, sizeof text);

    EXPECT_NULL(strstr(text, "Upgrading"));
    EXPECT_NOT_NULL(strstr(text, "one of the two versions has to"));
}

static bool answer_with(const char *typed) {
    FILE *in = tmpfile();
    FILE *out = tmpfile();
    if (in == NULL || out == NULL)
        return false;
    (void)fputs(typed, in);
    rewind(in);

    const bool accepted = conflict_prompt_ask(in, out);
    (void)fclose(in);
    (void)fclose(out);
    return accepted;
}

MOLTEST(the_question_defaults_to_yes_as_its_prompt_promises) {
    EXPECT_TRUE(answer_with("\n"));
    EXPECT_TRUE(answer_with("y\n"));
    EXPECT_TRUE(answer_with("Y\n"));
    EXPECT_FALSE(answer_with("n\n"));
    EXPECT_FALSE(answer_with("N\n"));
    /* Anything else is not a yes: this writes to a manifest. */
    EXPECT_FALSE(answer_with("maybe\n"));
    /* End of input is not an answer, and silence must not accept. */
    EXPECT_FALSE(answer_with(""));
}
