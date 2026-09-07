/* The per-boot generation comes from here. Seeded by the test so a failing run
 * reproduces, but still *different on every reset* -- which is the property the
 * generation exists for, and the one a fixed value would quietly break (it was
 * 2 on every boot once, and the guard against a phone resuming into a restarted
 * log never fired).
 */
#ifndef SHIM_ZEPHYR_RANDOM_RANDOM_H
#define SHIM_ZEPHYR_RANDOM_RANDOM_H

#include <stdint.h>

uint32_t sys_rand32_get(void);
void shim_rand_seed(uint32_t seed);

#endif /* SHIM_ZEPHYR_RANDOM_RANDOM_H */
