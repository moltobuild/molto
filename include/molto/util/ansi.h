#ifndef MOLTO_UTIL_ANSI_H
#define MOLTO_UTIL_ANSI_H

/*
 * The escape sequences Molto writes, in one place.
 *
 * They are here and not beside the code that uses them because two renderers
 * now paint the same build — the inventory and its bar, and the frame a
 * diagnostic is drawn in — and a build whose warning is a different yellow
 * from its bar reads as two programs sharing a terminal.
 *
 * Nothing here decides whether to emit any of it. That is a question about the
 * stream and the person at the end of it, and each renderer settles it once
 * for itself: a terminal is what makes colour worth writing, and NO_COLOR is
 * the person at that terminal saying they would rather read it plain.
 *
 * Every sequence below costs bytes and occupies no columns, which is why
 * padding is never counted over a string that contains one.
 */

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN "\033[36m"

#endif /* MOLTO_UTIL_ANSI_H */
