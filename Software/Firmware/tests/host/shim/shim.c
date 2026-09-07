/* The host stand-ins for the kernel, the logger and the flash part.
 *
 * See the headers for what each one deliberately does *not* do. The short
 * version: the flash only clears bits, and its operations can be hooked so a
 * single-threaded harness can put one thread's call inside another's.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/storage/flash_map.h>

/* ---- uptime -------------------------------------------------------------- */

static int64_t s_uptime;

int64_t k_uptime_get(void)
{
    return s_uptime;
}

void shim_uptime_set(int64_t ms)
{
    s_uptime = ms;
}

/* ---- random -------------------------------------------------------------- */

static uint32_t s_rand = 1;

void shim_rand_seed(uint32_t seed)
{
    s_rand = seed ? seed : 1;
}

uint32_t sys_rand32_get(void)
{
    /* xorshift32: deterministic per seed, and different on every call, which is
     * the only property the generation actually needs.
     */
    s_rand ^= s_rand << 13;
    s_rand ^= s_rand >> 17;
    s_rand ^= s_rand << 5;
    return s_rand;
}

/* ---- logging ------------------------------------------------------------- */

unsigned int shim_log_count[4];
char shim_log_last[4][256];
int shim_log_verbosity = 1;

static const char *s_watch;
unsigned int shim_log_watch_hits;

void shim_log_watch(const char *needle)
{
    s_watch = needle;
    shim_log_watch_hits = 0;
}

void shim_log_reset(void)
{
    memset(shim_log_count, 0, sizeof(shim_log_count));
    memset(shim_log_last, 0, sizeof(shim_log_last));
    shim_log_watch_hits = 0;
}

void shim_log(int level, const char *fmt, ...)
{
    static const char *const tag[4] = {"err", "wrn", "inf", "dbg"};
    va_list ap;

    if (level < 0 || level > 3) {
        return;
    }

    shim_log_count[level]++;

    va_start(ap, fmt);
    vsnprintf(shim_log_last[level], sizeof(shim_log_last[level]), fmt, ap);
    va_end(ap);

    if (s_watch != NULL && strstr(shim_log_last[level], s_watch) != NULL) {
        shim_log_watch_hits++;
    }

    if (shim_log_verbosity > 1 || (shim_log_verbosity == 1 && level <= 1)) {
        printf("    <%s> %s\n", tag[level], shim_log_last[level]);
    }
}

/* ---- flash --------------------------------------------------------------- */

static uint8_t *s_flash;
static size_t s_size;
static size_t s_page;
static struct flash_area s_area;
static struct device *s_dev = (struct device *)(uintptr_t)0x1;

unsigned int shim_flash_write_to_dirty;
unsigned int shim_flash_hook_fired;

static unsigned int s_hook_ops;
static void (*s_hook_cb)(void *);
static void *s_hook_arg;
static bool s_in_hook;

void shim_flash_hook(unsigned int ops, void (*cb)(void *), void *arg)
{
    s_hook_ops = ops;
    s_hook_cb = cb;
    s_hook_arg = arg;

    /* Arming resets the count; *disarming does not*. A test's last act is to
     * take the hook back off, and it then has to be able to ask whether the
     * thing it just set up actually happened -- "the hook never fired" is the
     * difference between a passing test and no test at all.
     */
    if (cb != NULL) {
        shim_flash_hook_fired = 0;
    }
}

/* Fired before the model does the work, so the callback sees the part exactly as
 * the operation found it. Re-entrancy is suppressed rather than allowed: a
 * callback that appends would otherwise fire the hook from inside itself and
 * recurse until the stack ran out.
 */
static void hook(enum shim_flash_op op)
{
    void (*cb)(void *) = s_hook_cb;

    if (cb == NULL || (s_hook_ops & (unsigned int)op) == 0 || s_in_hook) {
        return;
    }

    s_in_hook = true;
    shim_flash_hook_fired++;
    cb(s_hook_arg);
    s_in_hook = false;
}
unsigned int *shim_flash_erases;
size_t shim_flash_page_count;
int shim_flash_erase_fail_next;
int shim_flash_erase_noop_next;

void shim_flash_init(size_t size, size_t page_size)
{
    shim_flash_free();

    s_size = size;
    s_page = page_size;
    s_flash = malloc(size);
    if (s_flash == NULL) {
        fprintf(stderr, "shim: out of memory for a %zu-byte flash model\n", size);
        exit(1);
    }
    /* A part fresh out of the reel is not erased -- it is whatever was there.
     * Filling with a pattern rather than 0xFF is what makes "did the firmware
     * erase this page before writing to it?" a question with an answer.
     */
    memset(s_flash, 0xA5, size);

    shim_flash_page_count = size / page_size;
    free(shim_flash_erases);
    shim_flash_erases = calloc(shim_flash_page_count, sizeof(*shim_flash_erases));
    shim_flash_write_to_dirty = 0;
    shim_flash_erase_fail_next = 0;
    shim_flash_erase_noop_next = 0;
    s_hook_ops = 0;
    s_hook_cb = NULL;
    shim_flash_hook_fired = 0;

    s_area.fa_off = 0;
    s_area.fa_size = size;
}

void shim_flash_free(void)
{
    free(s_flash);
    s_flash = NULL;
    free(shim_flash_erases);
    shim_flash_erases = NULL;
    shim_flash_page_count = 0;
}

int flash_area_open(uint8_t id, const struct flash_area **fa)
{
    (void)id;
    if (s_flash == NULL) {
        return -19; /* -ENODEV */
    }
    *fa = &s_area;
    return 0;
}

const struct device *flash_area_get_device(const struct flash_area *fa)
{
    (void)fa;
    return s_dev;
}

int flash_get_page_info_by_offs(const struct device *dev, off_t offs, struct flash_pages_info *info)
{
    (void)offs;
    if (dev != s_dev) {
        return -19;
    }
    info->start_offset = 0;
    info->size = s_page;
    info->index = 0;
    return 0;
}

int flash_area_read(const struct flash_area *fa, off_t off, void *dst, size_t len)
{
    (void)fa;
    if (off < 0 || (size_t)off + len > s_size) {
        return -22; /* -EINVAL */
    }
    hook(SHIM_FLASH_READ);
    memcpy(dst, s_flash + off, len);
    return 0;
}

int flash_area_write(const struct flash_area *fa, off_t off, const void *src, size_t len)
{
    const uint8_t *in = src;

    (void)fa;
    if (off < 0 || (size_t)off + len > s_size) {
        return -22;
    }
    hook(SHIM_FLASH_WRITE);

    for (size_t i = 0; i < len; i++) {
        uint8_t was = s_flash[(size_t)off + i];

        /* The whole reason the model exists. A NOR write is an AND, so writing
         * over live data is not refused and does not read back as either value:
         * it reads back as nonsense that still parses. Count it and carry on --
         * carrying on is what the part does.
         */
        if ((was & in[i]) != in[i]) {
            shim_flash_write_to_dirty++;
        }
        s_flash[(size_t)off + i] = was & in[i];
    }
    return 0;
}

int flash_area_erase(const struct flash_area *fa, off_t off, size_t len)
{
    (void)fa;
    if (off < 0 || (size_t)off + len > s_size || (size_t)off % s_page != 0 || len % s_page != 0) {
        return -22;
    }
    if (shim_flash_erase_fail_next) {
        shim_flash_erase_fail_next = 0;
        return -5; /* -EIO */
    }
    hook(SHIM_FLASH_ERASE);

    if (shim_flash_erase_noop_next) {
        shim_flash_erase_noop_next = 0;
        return 0; /* "success", and the page is untouched */
    }

    memset(s_flash + off, 0xFF, len);
    for (size_t p = (size_t)off / s_page; p < ((size_t)off + len) / s_page; p++) {
        shim_flash_erases[p]++;
    }
    return 0;
}

unsigned int shim_flash_total_erases(void)
{
    unsigned int total = 0;

    for (size_t p = 0; p < shim_flash_page_count; p++) {
        total += shim_flash_erases[p];
    }
    return total;
}

bool shim_flash_page_is_erased(size_t page)
{
    for (size_t i = 0; i < s_page; i++) {
        if (s_flash[page * s_page + i] != 0xFF) {
            return false;
        }
    }
    return true;
}

/* ---- the one firmware symbol flash_store.c calls outside itself ---------- */

/* The console dump prints the battery line in the same {timestamp: ...} shape
 * as the records beside it, so it has to be on the same scale. On the ring that
 * is virtual uptime; here there is nothing to rebase, so it is plain uptime.
 */
uint32_t wallclock_uptime(void)
{
    return (uint32_t)k_uptime_get();
}

uint8_t battery_percent(uint16_t mv)
{
    return (uint8_t)(mv / 40);
}
