#ifndef MODE_SELECT_H_
#define MODE_SELECT_H_

#include <zephyr/kernel.h>
#include <stdbool.h>

typedef enum {
	APP_MODE_BRIDGE = 0,       /* Default: Pin High (Unbridged) -> Transparent UART <-> BLE */
	APP_MODE_PACKET_TNC = 1,   /* Pin Low (Grounded) -> AX.25 / KISS TNC Server */
	APP_MODE_DIGIPEATER = 2,   /* Digi Pin Low (Grounded) -> AX.25 Digipeater */
} app_mode_t;

#ifdef __cplusplus
extern "C" {
#endif

int mode_select_init(void);
app_mode_t mode_select_get_current(void);
const char *mode_select_get_name(app_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* MODE_SELECT_H_ */
