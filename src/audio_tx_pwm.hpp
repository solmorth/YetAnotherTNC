#ifndef AUDIO_TX_PWM_HPP
#define AUDIO_TX_PWM_HPP

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "mode_select.h"

#ifdef __cplusplus
extern "C" {
#endif

int audio_tx_pwm_init(void);
void audio_tx_set_gain(float gain);
float audio_tx_get_gain(void);
void audio_tx_set_fx25(bool enable);
bool audio_tx_get_fx25(void);
void audio_tx_pwm_play_packet(const uint8_t *data, size_t len, app_mode_t mode);

/* Bring-up/debug aid: keys PTT and plays a plain continuous 1200Hz tone for
 * a few seconds, bypassing BLE/KISS/AX.25/AFSK-framing entirely - isolates
 * whether the PWM DAC + RC filter + radio interface chain works at all,
 * without needing to catch a sub-second real packet with a scope/recorder.
 */
void audio_tx_pwm_test_tone(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_TX_PWM_HPP
