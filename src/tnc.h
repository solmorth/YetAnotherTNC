#ifndef TNC_H_
#define TNC_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include "mode_select.h"
#include "ptt.h"
#include "ax25.h"
#include "kiss.h"

typedef int (*ble_send_func_t)(const uint8_t *data, size_t len);

void tnc_init(void);
int tnc_tx_queue_init(const struct device *uart_dev);
int tnc_queue_tx_packet(const uint8_t *data, size_t len, app_mode_t mode);

void tnc_process_ble_bytes(const uint8_t *data, size_t len, app_mode_t mode, const struct device *uart_dev,
			    ble_send_func_t ble_send);

void tnc_process_radio_bytes(const uint8_t *data, size_t len, app_mode_t mode, const struct device *uart_dev, ble_send_func_t ble_send);

#endif /* TNC_H_ */
