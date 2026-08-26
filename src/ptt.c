#include "ptt.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include "log_ts.h"

#define PTT_PIN_NODE DT_NODELABEL(ptt_pin)

#if DT_NODE_EXISTS(PTT_PIN_NODE)
static const struct gpio_dt_spec ptt_spec = GPIO_DT_SPEC_GET(PTT_PIN_NODE, gpios);
#endif

static bool ptt_state = false;

int ptt_init(void)
{
	int ret = 0;

#if DT_NODE_EXISTS(PTT_PIN_NODE)
	if (gpio_is_ready_dt(&ptt_spec)) {
		ret = gpio_pin_configure_dt(&ptt_spec, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			printk("Failed to configure PTT pin: %d\n", ret);
			return ret;
		}
		printk("PTT Pin (Pin %d) initialized (Active-High / 3.3V on key, 0V idle)\n", ptt_spec.pin);
	} else {
		printk("Warning: PTT GPIO device not ready\n");
	}
#else
	printk("Warning: ptt_pin DT node not defined\n");
#endif

	ptt_state = false;
	return 0;
}

void ptt_set(bool active)
{
#if DT_NODE_EXISTS(PTT_PIN_NODE)
	if (gpio_is_ready_dt(&ptt_spec)) {
		gpio_pin_set_dt(&ptt_spec, active ? 1 : 0);
		ptt_state = active;
		printk("[PTT] Pin state set to: %s (Physical: %s)\n",
		       active ? "ACTIVE (3.3V)" : "INACTIVE (0V)",
		       active ? "HIGH" : "LOW");
	}
#endif
}

bool ptt_is_active(void)
{
	return ptt_state;
}
