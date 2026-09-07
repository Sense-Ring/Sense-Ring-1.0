/* Just enough of Zephyr's util.h to compile vitals.c on a host compiler.
 *
 * vitals.c is pure arithmetic -- no kernel calls, no drivers, no hardware -- so
 * the only thing standing between it and a host test is this header. Shimming
 * it is deliberate: the alternative is building the whole Zephyr tree to test
 * one file of integer maths, which is slow enough that nobody would run it.
 *
 * Keep this minimal. Anything added here is a divergence between what the tests
 * exercise and what the firmware compiles against, so if vitals.c ever needs a
 * real Zephyr facility, that is a signal to move the test rather than to grow
 * this file.
 */
#ifndef SHIM_ZEPHYR_SYS_UTIL_H
#define SHIM_ZEPHYR_SYS_UTIL_H

#include <assert.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef CLAMP
#define CLAMP(val, low, high) (((val) <= (low)) ? (low) : MIN(val, high))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Zephyr's is a compile-time assert with a message; static_assert matches. */
#ifndef BUILD_ASSERT
#define BUILD_ASSERT(cond, ...) static_assert(cond, "" __VA_ARGS__)
#endif

#endif /* SHIM_ZEPHYR_SYS_UTIL_H */
