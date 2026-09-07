/* The one thing flash_store.c wants from the flash driver: the page size.
 *
 * It asks rather than assuming, which is worth preserving in the shim -- the
 * tests run the same query, so a partition/page combination that does not tile
 * a 16-byte record is rejected by the same check the firmware uses.
 */
#ifndef SHIM_ZEPHYR_DRIVERS_FLASH_H
#define SHIM_ZEPHYR_DRIVERS_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct device;

struct flash_pages_info {
    off_t start_offset;
    size_t size;
    uint32_t index;
};

int flash_get_page_info_by_offs(const struct device *dev, off_t offs, struct flash_pages_info *info);

#endif /* SHIM_ZEPHYR_DRIVERS_FLASH_H */
