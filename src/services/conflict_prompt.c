#include <molto/services/conflict_prompt.h>

#include <molto/project/manifest_edit.h>
#include <molto/services/fs_service.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The arrow RFC-0008 draws the two claims with. Spelled as its bytes so the
   source stays ASCII, like every other string molto prints. */
#define CONFLICT_ARROW "\xe2\x86\x90"

#define PROMPT_PATH_MAX 1024

/* Who required something. The root package is named by its relationship to the
   reader rather than by its name: they are the ones being asked. */
static void describe_requirer(const char *required_by, char *out, size_t out_size) {
    if(required_by[0] == '\0')
        snprintf(out, out_size, "required by this project");
    else
        snprintf(out, out_size, "required by %s", required_by);
}

static size_t append(char *out, size_t out_size, size_t used, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

static size_t append(char *out, size_t out_size, size_t used, const char *format, ...) {
    if(used >= out_size)
        return used;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(out + used, out_size - used, format, args);
    va_end(args);
    return written < 0 ? used : used + (size_t)written;
}

void conflict_prompt_render(const dep_conflict *conflict, char *out, size_t out_size) {
    char first[128] = "";
    char second[128] = "";
    describe_requirer(conflict->required_by, first, sizeof first);
    describe_requirer(conflict->other_required_by, second, sizeof second);

    size_t used = 0;
    used = append(out, out_size, used, "molto: %s is required at two versions\n", conflict->name);
    used = append(out, out_size, used, "    %s  " CONFLICT_ARROW " %s\n", conflict->version,
                  first);
    used = append(out, out_size, used, "    %s  " CONFLICT_ARROW " %s\n", conflict->other_version,
                  second);

    if(conflict->has_proposal) {
        (void)append(
            out, out_size, used, "\n  Upgrading %s to %s requires %s %s and resolves this.\n",
            conflict->change_name, conflict->change_to, conflict->name, conflict->settles_on);
    } else {
        /* No proposal is not a dead end: the user still has two versions they
           declared and can change either one. Saying so is the message's job. */
        (void)append(out, out_size, used,
                     "\n  Nothing molto can change removes this: one of the two versions has to.\n");
    }
}

bool conflict_prompt_ask(FILE *in, FILE *out) {
    /* The question goes to stderr with the message it belongs to, so a caller
       redirecting stdout still sees what it is being asked. */
    fprintf(out, "\n  Apply? [Y/n] ");
    (void)fflush(out);

    char answer[16] = "";
    if(fgets(answer, sizeof answer, in) == NULL)
        return false;
    if(answer[0] == '\n' || answer[0] == '\0')
        return true;
    return answer[0] == 'y' || answer[0] == 'Y';
}

bool conflict_prompt_apply(const char *root, const dep_conflict *conflict) {
    char message[1024] = "";
    conflict_prompt_render(conflict, message, sizeof message);
    fputs(message, stderr);

    if(!conflict->has_proposal)
        return false;
    if(!isatty(STDIN_FILENO)) {
        fprintf(stderr,
                "\n  Not applied: molto does not choose a version nobody typed. Write it into "
                "Project.toml, or run this where the question can be answered.\n");
        return false;
    }
    if(!conflict_prompt_ask(stdin, stderr))
        return false;

    char path[PROMPT_PATH_MAX];
    char value[DEP_VERSION_MAX + 4];
    char err[256] = "";
    if(!fs_format_path(path, sizeof path, "%s/Project.toml", root)) {
        fprintf(stderr, "molto: the path to Project.toml is too long\n");
        return false;
    }
    snprintf(value, sizeof value, "\"%s\"", conflict->change_to);

    /* The same edit `molto add` makes, through the same line-level editor: the
       entry is replaced where the user put it, and comments survive. */
    if(!manifest_add_dep(path, conflict->change_table, conflict->change_name, value, err,
                         sizeof err)) {
        fprintf(stderr, "molto: %s\n", err);
        return false;
    }
    fprintf(stderr, "  %s = %s written to [%s]\n", conflict->change_name, value,
            conflict->change_table);
    return true;
}
