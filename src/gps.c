#include "gps.h"
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include "log_ts.h"

#define GPS_ENABLE_PIN_NODE DT_NODELABEL(gps_enable_pin)
#define GPS_UART_NODE DT_NODELABEL(uart1)

#if DT_NODE_EXISTS(GPS_ENABLE_PIN_NODE)
static const struct gpio_dt_spec gps_enable_spec = GPIO_DT_SPEC_GET(GPS_ENABLE_PIN_NODE, gpios);
#endif

static const struct device *const gps_uart_dev = DEVICE_DT_GET(GPS_UART_NODE);

static bool gps_enabled = false;

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
