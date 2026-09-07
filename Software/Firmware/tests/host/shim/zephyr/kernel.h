/* Just enough of Zephyr's kernel.h to compile flash_store.c on a host compiler.
 *
 * The same bargain the util.h shim makes, and for a file that earns it more:
 * flash_store.c is ring-buffer arithmetic over a flash driver, and the ring
 * arithmetic is the single riskiest thing in the tree. It fails by corrupting
 * records rather than by crashing, so
 * the only way to catch it is to walk the log back and check, and doing that on
 * hardware costs 77 minutes of unbroken finger contact per lap.
 *
 * Keep this minimal, and keep it *honest*: every stub here is a place where the
 * test and the firmware diverge. There is exactly one left that matters, and it
 * is documented in flash_map.h -- the flash is a model, and it only clears bits.
 *
 * **This shrank when the firmware did.** It used to carry a whole fake work
 * queue, because the erase ran on one; the firmware now does that work on the
 * measurement thread, so there is nothing here to fake. That is the shape to
 * keep: a shim that has to grow a scheduler is usually telling you the code
 * under test has grown a thread it did not need.
 */
#ifndef SHIM_ZEPHYR_KERNEL_H
#define SHIM_ZEPHYR_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <zephyr/sys/util.h>

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

#ifndef ARG_UNUSED
#define ARG_UNUSED(x) ((void)(x))
#endif

/* Milliseconds since boot. The tests drive it so timestamps are reproducible. */
int64_t k_uptime_get(void);
void shim_uptime_set(int64_t ms);

#endif /* SHIM_ZEPHYR_KERNEL_H */
