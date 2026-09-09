#include <molto/cli.h>

#include <molto/services/console_service.h>

/* The console is made ready before anything prints and put back after, because
   what the pair changes on Windows belongs to the window rather than to this
   process and would otherwise outlive the command. On POSIX both calls are
   nothing, by design: see console_output_prepare. */
int main(int argc, char **argv) {
    const console_output original = console_output_prepare();
    const int status = cli_run(argc, argv);
    console_output_restore(original);
    return status;
}
