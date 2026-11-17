/* TARGET ONLY. A minimal SEGGER RTT up-channel: a log console over SWD that
 * needs no pins and no UART. The J-Link finds the control block by scanning
 * RAM for its ID; read it with `JLinkRTTClient` or `JLinkRTTLogger`.
 *
 * Implements the documented control-block layout (one up buffer, one down
 * buffer), non-blocking: when the host is not reading and the buffer is
 * full, output is dropped rather than stalling the meter.
 */
#ifndef RTT_H
#define RTT_H

#include <stddef.h>

void   rtt_init(void);
size_t rtt_write(const char *s, size_t n);
void   rtt_puts(const char *s);
void   rtt_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
