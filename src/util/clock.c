#include <molto/util/clock.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

double clock_monotonic_seconds(void) {
#ifdef _WIN32
    /* The frequency is fixed for the life of the process, so it is asked for
       once. Windows has no monotonic clock in <time.h>; this counter is the
       one that does not jump. */
    static LARGE_INTEGER frequency = {0};
    if(frequency.QuadPart == 0 && QueryPerformanceFrequency(&frequency) == 0)
        return 0.0;

    LARGE_INTEGER now;
    if(QueryPerformanceCounter(&now) == 0)
        return 0.0;
    return (double)now.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
#endif
}
