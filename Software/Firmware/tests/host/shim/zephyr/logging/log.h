/* Zephyr's logging, shimmed to printf and a counter.
 *
 * The counting is not decoration. Several of the things flash_store.c must do
 * are *only* observable as a log line -- "Log lapped: dropping N unacknowledged
 * byte(s)" is the record of data loss, and "Empty slot inside the live range"
 * is the pointers having drifted from what is in flash -- so the tests assert on
 * whether those fired. A warning nobody counts is a warning nobody checks.
 */
#ifndef SHIM_ZEPHYR_LOGGING_LOG_H
#define SHIM_ZEPHYR_LOGGING_LOG_H

#include <stdio.h>

#define LOG_LEVEL_ERR 1
#define LOG_LEVEL_WRN 2
#define LOG_LEVEL_INF 3
#define LOG_LEVEL_DBG 4

#define LOG_MODULE_REGISTER(...)
#define LOG_MODULE_DECLARE(...)

/* Counts by severity, plus the text of the last line at each, so a test can say
 * which warning it expected rather than only how many there were.
 */
extern unsigned int shim_log_count[4];
extern char shim_log_last[4][256];

void shim_log(int level, const char *fmt, ...);
void shim_log_reset(void);

/* Counts every line containing `needle` until the next shim_log_watch(). Some
 * of what this module does is only ever observable as one particular line --
 * "Log lapped: dropping N unacknowledged byte(s)" is the record that a wearer's
 * data was destroyed -- and counting lines by severity cannot distinguish that
 * from any other warning.
 */
void shim_log_watch(const char *needle);
extern unsigned int shim_log_watch_hits;

/* 0 prints nothing, 1 prints warnings and errors, 2 prints everything. The log
 * is a few lines per record and the soak writes 29,000 of them.
 */
extern int shim_log_verbosity;

#define LOG_ERR(...) shim_log(0, __VA_ARGS__)
#define LOG_WRN(...) shim_log(1, __VA_ARGS__)
#define LOG_INF(...) shim_log(2, __VA_ARGS__)
#define LOG_DBG(...) shim_log(3, __VA_ARGS__)

#define SHIM_LOG_ERR 0
#define SHIM_LOG_WRN 1
#define SHIM_LOG_INF 2
#define SHIM_LOG_DBG 3

#endif /* SHIM_ZEPHYR_LOGGING_LOG_H */
