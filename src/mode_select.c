#include "mode_select.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include "log_ts.h"

#define MODE_PIN_NODE DT_NODELABEL(mode_pin)
#define DIGI_PIN_NODE DT_NODELABEL(digi_pin)

#if DT_NODE_EXISTS(MODE_PIN_NODE)
static const struct gpio_dt_spec mode_spec = GPIO_DT_SPEC_GET(MODE_PIN_NODE, gpios);
#endif

#if DT_NODE_EXISTS(DIGI_PIN_NODE)
static const struct gpio_dt_spec digi_spec = GPIO_DT_SPEC_GET(DIGI_PIN_NODE, gpios);
#endif

static app_mode_t current_mode = APP_MODE_BRIDGE;

int mode_select_init(void)
{
	int ret = 0;
	int err;

#if DT_NODE_EXISTS(MODE_PIN_NODE)
	if (gpio_is_ready_dt(&mode_spec)) {
		err = gpio_pin_configure_dt(&mode_spec, GPIO_INPUT);
		if (err < 0) {
			printk("Failed to configure mode pin: %d\n", err);
			ret = err;
		}
	}
#endif

#if DT_NODE_EXISTS(DIGI_PIN_NODE)
	if (gpio_is_ready_dt(&digi_spec)) {
		err = gpio_pin_configure_dt(&digi_spec, GPIO_INPUT);
		if (err < 0) {
			printk("Failed to configure digi pin: %d\n", err);
			ret = err;
		}
	}
#endif

#if DT_NODE_EXISTS(MODE_PIN_NODE)
	if (gpio_is_ready_dt(&mode_spec)) {
		printk("Mode Pin (Pin %d) logical state: %d\n", mode_spec.pin, gpio_pin_get_dt(&mode_spec));
	}
#endif

#if DT_NODE_EXISTS(DIGI_PIN_NODE)
	if (gpio_is_ready_dt(&digi_spec)) {
		printk("Digi Pin (Pin %d) logical state: %d\n", digi_spec.pin, gpio_pin_get_dt(&digi_spec));
	}
#endif

	current_mode = mode_select_get_current();
	printk("Hardware Mode Selected: %s\n", mode_select_get_name(current_mode));

	return ret;
}

app_mode_t mode_select_get_current(void)
{
	bool mode_active = false;
	bool digi_active = false;

#if DT_NODE_EXISTS(MODE_PIN_NODE)
	if (gpio_is_ready_dt(&mode_spec)) {
		mode_active = (gpio_pin_get_dt(&mode_spec) > 0);
	}
#endif

#if DT_NODE_EXISTS(DIGI_PIN_NODE)
	if (gpio_is_ready_dt(&digi_spec)) {
		digi_active = (gpio_pin_get_dt(&digi_spec) > 0);
	}
#endif

	if (digi_active) {
		return APP_MODE_DIGIPEATER;
	} else if (mode_active) {
		return APP_MODE_PACKET_TNC;
	} else {
		return APP_MODE_BRIDGE;
	}
}

const char *mode_select_get_name(app_mode_t mode)
{
	switch (mode) {
	case APP_MODE_BRIDGE:
		return "BRIDGE (Transparent UART <-> BLE)";
	case APP_MODE_PACKET_TNC:
		return "PACKET_TNC (KISS / AX.25 Server)";
	case APP_MODE_DIGIPEATER:
		return "DIGIPEATER (AX.25 Repeater + BLE)";
	default:
		return "UNKNOWN";
	}
}
