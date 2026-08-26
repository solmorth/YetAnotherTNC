/* Prefixes every printk() line with seconds.millis since boot (journalctl-style),
 * since USB CDC ACM has no other timestamp and boot time varies run to run. */
#ifndef LOG_TS_H_
#define LOG_TS_H_

#include <stdarg.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static inline void printk_ts(const char *fmt, ...)
{
	uint32_t ms = (uint32_t)k_uptime_get();

	printk("[%5u.%03u] ", ms / 1000, ms % 1000);

	va_list args;
	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
}

#define printk(fmt, ...) printk_ts(fmt, ##__VA_ARGS__)

#endif
