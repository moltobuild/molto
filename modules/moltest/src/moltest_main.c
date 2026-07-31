#include <moltest.h>

/* Default entry point for a moltest binary: run every registered test. Link
   this file in and no main() of your own is needed. */
int main(int argc, char **argv) {
    return moltest_run(argc, argv);
}
