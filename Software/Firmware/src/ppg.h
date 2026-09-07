#ifndef PPG_H
#define PPG_H

#include <stddef.h>
#include <stdint.h>

/* Output sample rate: the chip converts at 50Hz and averages pairs. Everything
 * downstream derives its timing from this, so it lives here rather than being
 * spelled out twice. See ARCHITECTURE.md §3.2.
 */
#define PPG_SAMPLE_RATE_HZ 25

/* One FIFO entry: 18-bit ADC counts from each LED channel. */
struct ppg_sample {
    uint32_t red;
    uint32_t ir;
};

/* Verifies the MAX30102 and loads its configuration. Leaves the sensor stopped
 * and the VLED rail down -- call ppg_start() to take a measurement.
 */
int ppg_init(void);

/* Raises the VLED rail, wakes the sensor and clears the FIFO so the window
 * starts on settled data. Returns 0 or a negative errno.
 */
int ppg_start(void);

/* Drains whole samples from the sensor FIFO. Returns the number of samples
 * read (possibly 0), or a negative errno.
 */
int ppg_read_fifo(struct ppg_sample *samples, size_t max_samples);

/* Shuts the sensor down and drops the VLED rail. */
void ppg_stop(void);

#endif
