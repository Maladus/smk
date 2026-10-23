#pragma once

#include "console.h"

// Console output, by log level. A call site picks the level it belongs to and
// levels above DEBUG_LOG_LEVEL compile out completely, format strings included,
// so a quieter image is a smaller image:
//   1  banner and hard errors (boot, an interrupt vector with no handler)
//   2  status changes (RF link, pairing, settings saves, battery)
//   3  dumps and traces (report range errors, part/serial dump, interrupt stats)
// Set it per build with `-Dlog_level=1..3` (meson option).
#if DEBUG == 1
#    ifndef DEBUG_LOG_LEVEL
#        define DEBUG_LOG_LEVEL 3
#    endif
#    define dprintf(...) console_printf(__VA_ARGS__)
#    if DEBUG_LOG_LEVEL >= 1
#        define dlog1(...) dprintf(__VA_ARGS__)
#    else
#        define dlog1(...) ((void)0)
#    endif
#    if DEBUG_LOG_LEVEL >= 2
#        define dlog2(...) dprintf(__VA_ARGS__)
#    else
#        define dlog2(...) ((void)0)
#    endif
#    if DEBUG_LOG_LEVEL >= 3
#        define dlog3(...) dprintf(__VA_ARGS__)
#    else
#        define dlog3(...) ((void)0)
#    endif
#else
#    define dprintf(...) ((void)0)
#    define dlog1(...)   ((void)0)
#    define dlog2(...)   ((void)0)
#    define dlog3(...)   ((void)0)
#endif

// Use instead of an `if` around dprintf: an empty branch trips --Werror.
#if DEBUG == 1
#    define dprintf_if(cond, ...)           \
        do {                                \
            if (cond) dprintf(__VA_ARGS__); \
        } while (0)
#else
#    define dprintf_if(cond, ...) ((void)(cond))
#endif
