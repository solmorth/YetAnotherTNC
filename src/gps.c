#include "gps.h"
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>
#include "log_ts.h"

#define GPS_ENABLE_PIN_NODE DT_NODELABEL(gps_enable_pin)
#define GPS_UART_NODE DT_NODELABEL(uart1)

#if DT_NODE_EXISTS(GPS_ENABLE_PIN_NODE)
static const struct gpio_dt_spec gps_enable_spec = GPIO_DT_SPEC_GET(GPS_ENABLE_PIN_NODE, gpios);
#endif

static const struct device *const gps_uart_dev = DEVICE_DT_GET(GPS_UART_NODE);

static bool gps_enabled = false;

/* NMEA sentences arrive as short CRLF-terminated lines at a few Hz. Batch-
 * drain them on a short idle timer rather than printk-ing from the ISR:
 * printk() over USB CDC ACM can block indefinitely if called from interrupt
 * context, which freezes the whole system (same hazard as uart_isr() in
 * main.c).
 */
#define GPS_RX_RING_BUF_SIZE 256
#define GPS_RX_IDLE_TIMEOUT_MS 50

RING_BUF_DECLARE(gps_rx_ring_buf, GPS_RX_RING_BUF_SIZE);

static void gps_rx_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	uint8_t buf[GPS_RX_RING_BUF_SIZE + 1];
	uint32_t len = ring_buf_get(&gps_rx_ring_buf, buf, GPS_RX_RING_BUF_SIZE);

	if (len > 0) {
		buf[len] = '\0';
		printk("[GPS] %s\n", buf);
	}
}
static K_WORK_DELAYABLE_DEFINE(gps_rx_work, gps_rx_work_handler);

static void gps_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(dev);

	if (uart_irq_rx_ready(dev)) {
		uint8_t buf[32];
		int len;

		while ((len = uart_fifo_read(dev, buf, sizeof(buf))) > 0) {
			ring_buf_put(&gps_rx_ring_buf, buf, len);
		}

		k_work_reschedule(&gps_rx_work, K_MSEC(GPS_RX_IDLE_TIMEOUT_MS));
	}
}

int gps_init(void)
{
	int ret = 0;

#if DT_NODE_EXISTS(GPS_ENABLE_PIN_NODE)
	if (gpio_is_ready_dt(&gps_enable_spec)) {
		ret = gpio_pin_configure_dt(&gps_enable_spec, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			printk("Failed to configure GPS enable pin: %d\n", ret);
			return ret;
		}
		printk("GPS Enable Pin (Pin %d) initialized (default LOW / disabled)\n",
		       gps_enable_spec.pin);
	} else {
		printk("Warning: GPS enable GPIO device not ready\n");
	}
#else
	printk("Warning: gps_enable_pin DT node not defined\n");
#endif

	if (!device_is_ready(gps_uart_dev)) {
		printk("Warning: GPS UART (uart1) device not ready\n");
	} else {
		printk("GPS UART (uart1) ready\n");

		int uart_ret = uart_irq_callback_user_data_set(gps_uart_dev, gps_uart_isr, NULL);
		if (uart_ret < 0) {
			printk("Warning: GPS UART interrupt-driven API not supported: %d\n",
			       uart_ret);
		} else {
			uart_irq_rx_enable(gps_uart_dev);
		}
	}

	gps_enabled = false;
	return ret;
}

void gps_enable_set(bool enable)
{
#if DT_NODE_EXISTS(GPS_ENABLE_PIN_NODE)
	if (gpio_is_ready_dt(&gps_enable_spec)) {
		gpio_pin_set_dt(&gps_enable_spec, enable ? 1 : 0);
		gps_enabled = enable;
		printk("[GPS] Enable pin set to: %s\n", enable ? "HIGH (enabled)" : "LOW (disabled)");
	}
#endif
}

bool gps_is_enabled(void)
{
	return gps_enabled;
}
