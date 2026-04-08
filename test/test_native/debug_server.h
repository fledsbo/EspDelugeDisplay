// Stub for native unit tests — debug_log is a no-op
#pragma once
#include <stdarg.h>
#include <stdio.h>

inline void debug_log(const char* fmt, ...) {
    // silent in tests — uncomment for debugging:
    // va_list args; va_start(args, fmt); vprintf(fmt, args); va_end(args); printf("\n");
}
