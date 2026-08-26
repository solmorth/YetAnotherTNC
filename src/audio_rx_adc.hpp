#ifndef AUDIO_RX_ADC_HPP
#define AUDIO_RX_ADC_HPP

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*ble_send_func_t)(const uint8_t *data, size_t len);

int audio_rx_adc_init(ble_send_func_t ble_send_cb);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_RX_ADC_HPP
