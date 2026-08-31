#include "audio_rx_adc.hpp"
#include "afsk_demodulator.hpp"
#include "kiss.h"
#include "mode_select.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/printk.h>
#include "log_ts.h"
#include <vector>

#define RX_AUDIO_ADC_NODE DT_PATH(zephyr_user)

#if DT_NODE_EXISTS(RX_AUDIO_ADC_NODE)
static const struct adc_dt_spec rx_audio_adc = ADC_DT_SPEC_GET_BY_IDX(RX_AUDIO_ADC_NODE, 0);
#endif

static ble_send_func_t g_ble_send_cb = NULL;
static AFSKDemodulator g_demod(9600, 1200);

/* Raw SAADC counts are biased around whatever DC level the analog front
 * end centers the audio signal at (not necessarily VDD/2, depends on the
 * bias network on Pin 31) - track and remove it with a slow leaky
 * integrator so process_sample() sees an AC-coupled, roughly zero-centered
 * signal the way it did from the old digital GPIO input. alpha is small
 * enough to leave 1200/2200 Hz AFSK tones untouched while still tracking
 * bias drift.
 *
 * ADC_COUNTS_TO_UNITY is a calibration knob, not a measured constant: the
 * demodulator's discriminator math (see afsk_demodulator.cpp) assumes a
 * roughly unit-amplitude input, but the actual peak swing out of the radio's
 * discriminator/audio-in circuit depends on hardware gain that varies board
 * to board. Tune this against real signal if decode quality is poor.
 */
#define ADC_DC_TRACK_ALPHA 0.001f
#define ADC_COUNTS_TO_UNITY 600.0f
static float g_dc_estimate = 2048.0f; /* mid-scale start for a 12-bit ADC */

#define RX_THREAD_STACK_SIZE 3072 /* FX.25 RS(255,239) decode needs headroom beyond plain AX.25 */
#define RX_THREAD_PRIORITY 6

static void rx_audio_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_thread_name_set(k_current_get(), "rx_afsk");

	std::vector<uint8_t> payload;
	int16_t adc_raw = 0;
	int16_t window_min = 0, window_max = 0;
	uint32_t sample_count = 0;

	struct adc_sequence sequence = {
		.buffer = &adc_raw,
		.buffer_size = sizeof(adc_raw),
	};

	while (1) {
		app_mode_t mode = mode_select_get_current();
		if (mode == APP_MODE_PACKET_TNC || mode == APP_MODE_DIGIPEATER ||
		    mode == APP_MODE_STANDALONE) {
#if DT_NODE_EXISTS(RX_AUDIO_ADC_NODE)
			if (adc_is_ready_dt(&rx_audio_adc) &&
			    adc_sequence_init_dt(&rx_audio_adc, &sequence) == 0 &&
			    adc_read_dt(&rx_audio_adc, &sequence) == 0) {
				g_dc_estimate += ((float)adc_raw - g_dc_estimate) * ADC_DC_TRACK_ALPHA;
				float ac = (float)adc_raw - g_dc_estimate;

				/* Bring-up diagnostic: with no signal (or bad bias) this
				 * stays near zero - a real AFSK tone swings noticeably
				 * whenever a packet is on the air.
				 */
				int16_t ac_i = (int16_t)ac;
				if (sample_count == 0 || ac_i < window_min) {
					window_min = ac_i;
				}
				if (sample_count == 0 || ac_i > window_max) {
					window_max = ac_i;
				}
				if (++sample_count >= 9600) {
					int16_t pp = window_max - window_min;
					if (pp > 8) {
						printk("[AFSK RX] Pin 31 activity: %d counts peak-to-peak\n", pp);
					}
					sample_count = 0;
				}

				float sample = ac / ADC_COUNTS_TO_UNITY;

				if (g_demod.process_sample(sample, payload)) {
					int fx25_corrections = g_demod.last_fx25_corrections();
					if (fx25_corrections >= 0) {
						printk("[AFSK RX] Decoded AX.25 Frame via FX.25 FEC, len %d (%d byte errors corrected)\n",
						       (int)payload.size(), fx25_corrections);
					} else {
						printk("[AFSK RX] Decoded AX.25 Frame, len %d\n", (int)payload.size());
					}

					/* Wrap decoded payload into KISS frame and send over BLE NUS */
					uint8_t kiss_buf[512];
					int kiss_len = kiss_encode_frame(KISS_CMD_DATA, payload.data(), payload.size(), kiss_buf, sizeof(kiss_buf));

					if (kiss_len > 0 && g_ble_send_cb != NULL) {
						g_ble_send_cb(kiss_buf, kiss_len);
					}
					payload.clear();
				}
			}
#endif
		}
		k_usleep(104); // 9600 Hz sampling interval (104 us)
	}
}

K_THREAD_DEFINE(rx_audio_thread_id, RX_THREAD_STACK_SIZE, rx_audio_thread_entry, NULL, NULL, NULL, RX_THREAD_PRIORITY, 0, 0);

extern "C" int audio_rx_adc_init(ble_send_func_t ble_send_cb)
{
	g_ble_send_cb = ble_send_cb;
#if DT_NODE_EXISTS(RX_AUDIO_ADC_NODE)
	if (adc_is_ready_dt(&rx_audio_adc) && adc_channel_setup_dt(&rx_audio_adc) == 0) {
		printk("RX Audio Pin 31 ADC initialized (9600 Hz AFSK Demodulator thread active)\n");
	} else {
		printk("ERROR: RX Audio ADC (Pin 31) not ready\n");
	}
#endif
	return 0;
}
