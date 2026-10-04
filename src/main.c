#include <molto/cli.h>

#include <molto/services/console_service.h>
#include <molto/services/paths_service.h>

#include <stdio.h>
#include <string.h>

/* What an older molto left in ~/.molto, moved before any command looks for it.
   Here rather than in cli_run so that only the program does it: the suite
   drives cli_run directly, and must never move the developer's own files. */
static void migrate_legacy_home(void) {
    char note[2048];
    if(!paths_migrate_legacy_home(note, sizeof note))
        return;
    for(char *line = strtok(note, "\n"); line != NULL; line = strtok(NULL, "\n"))
        fprintf(stderr, "molto: %s\n", line);
}

/* The console is made ready before anything prints and put back after, because
   what the pair changes on Windows belongs to the window rather than to this
   process and would otherwise outlive the command. On POSIX both calls are
   nothing, by design: see console_output_prepare. */
int main(int argc, char **argv) {
    const console_output original = console_output_prepare();
    migrate_legacy_home();
    const int status = cli_run(argc, argv);
    console_output_restore(original);
    return status;
}
