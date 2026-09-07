/* The flash map API, over a modelled NOR part.
 *
 * The model is the point of the whole harness, so it is worth saying what it
 * models: **NOR flash can only clear bits.** Erasing sets a page to 0xFF;
 * writing ANDs. So writing a record into a slot that was not erased first does
 * not fail and does not look wrong at the API -- it silently produces a record
 * that is the bitwise AND of two readings. That is precisely the failure mode
 * the ring arithmetic has to prevent and precisely the one that would never be
 * caught by a model that let writes overwrite.
 *
 * shim_flash_write_to_dirty counts every time it happens, and every test
 * asserts it is zero.
 */
#ifndef SHIM_ZEPHYR_STORAGE_FLASH_MAP_H
#define SHIM_ZEPHYR_STORAGE_FLASH_MAP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <zephyr/drivers/flash.h>

struct flash_area {
    off_t fa_off;
    size_t fa_size;
};

/* The DTS gives the ring one storage partition; the model gives it one id. */
#define FIXED_PARTITION_ID(label) 1

int flash_area_open(uint8_t id, const struct flash_area **fa);
int flash_area_read(const struct flash_area *fa, off_t off, void *dst, size_t len);
int flash_area_write(const struct flash_area *fa, off_t off, const void *src, size_t len);
int flash_area_erase(const struct flash_area *fa, off_t off, size_t len);
const struct device *flash_area_get_device(const struct flash_area *fa);

/* ---- what the model records ---------------------------------------------- */

/* Sized and (re)initialised by the test. `page_size` must divide `size`. */
void shim_flash_init(size_t size, size_t page_size);
void shim_flash_free(void);

/* A write into a slot that was not erased first: silent corruption on a real
 * part, and always a test failure here.
 */
extern unsigned int shim_flash_write_to_dirty;

/* Erases per page, indexed by page number. The wear budget is ~10k cycles per
 * page, and "is any page erased twice per lap?" is a question about this array.
 */
extern unsigned int *shim_flash_erases;
extern size_t shim_flash_page_count;

/* Set to make the next erase fail, so the "a failed erase must not advance the
 * frontier" path can be exercised rather than reasoned about.
 */
extern int shim_flash_erase_fail_next;

/* A nastier failure, and the one worth modelling: the next erase **reports
 * success and does nothing.** Real parts do this when they wear out, and it is
 * strictly worse than an error return, because everything downstream believes
 * the page is clean. Exists to test that the firmware reads back rather than
 * trusting the driver.
 */
extern int shim_flash_erase_noop_next;

/* ---- interleaving -------------------------------------------------------- */

/* Runs `cb` at the *start* of every flash operation of the named kinds, before
 * the model does the work.
 *
 * This is how a single-threaded harness tests a module that is not called from
 * a single thread. `flash_store_release()` runs on the Bluetooth RX thread and
 * can land at any instant relative to the measurement thread's work; the walk in
 * `flash_store_foreach()` runs on the replay work queue and overlaps appends.
 * Hooking the flash operations puts the other thread's call *inside* main's,
 * deterministically, which is the interleaving that matters -- the flash
 * operations are the only things here that take long enough for another thread
 * to get a turn during them.
 *
 * It is not a substitute for a scheduler and does not pretend to be: it cannot
 * interleave between two ordinary statements. What it covers is the class of
 * bug where a pointer moved underneath an operation already in progress.
 *
 * Re-entrant calls do not re-fire the hook, so a callback may freely append,
 * acknowledge or walk.
 */
enum shim_flash_op {
    SHIM_FLASH_READ = 1,
    SHIM_FLASH_WRITE = 2,
    SHIM_FLASH_ERASE = 4,
};

void shim_flash_hook(unsigned int ops, void (*cb)(void *), void *arg);
extern unsigned int shim_flash_hook_fired;

unsigned int shim_flash_total_erases(void);

/* True if every byte of the page is 0xFF -- "has this been scrubbed?" */
bool shim_flash_page_is_erased(size_t page);

#endif /* SHIM_ZEPHYR_STORAGE_FLASH_MAP_H */
