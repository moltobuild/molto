#ifndef MOLTO_CLOCK_H
#define MOLTO_CLOCK_H

/*
 * Seconds on a clock that only moves forward.
 *
 * The origin is unspecified and deliberately useless: the only thing to do
 * with one reading is subtract it from another. What matters is that the
 * clock does not jump — a build that takes four seconds must not report minus
 * three thousand because ntp corrected the wall clock while it ran.
 *
 * Molto spelled this `clock_gettime(CLOCK_MONOTONIC, ...)` until the Windows
 * port. mingw *declares* that function and does not provide it, so the call
 * compiled cleanly and failed at link — which is why the RFC-0017 workflow
 * builds rather than syntax-checks.
 */
[[nodiscard]] double clock_monotonic_seconds(void);

#endif /* MOLTO_CLOCK_H */
