#include "audio_tx_pwm.hpp"
#include "afsk_modulator.hpp"
#include "ptt.h"
#include <cmath>
#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/printk.h>
#include "log_ts.h"

/*
 * TX Audio DAC: nRF52840 hardware PWM (PWM0 channel 1) drives Pin 20.
 * The peripheral free-runs a fixed-frequency carrier in hardware; this
 * code only rewrites the duty-cycle compare register once per AFSK audio
 * sample (9600 Hz). That gives a true duty-modulated pulse train at a
 * carrier far above the audio band, which an external RC low-pass on
 * Pin 20 reconstructs into the analog AFSK tone - unlike bit-banging the
 * pin directly at the sample rate, which used the audio rate itself as
 * the "carrier" and left little room for a passive filter to separate
 * ripple from signal.
 */
#define TX_PWM_NODE DT_NODELABEL(pwm0)
#define TX_PWM_CHANNEL 1

/* 16 MHz PWM clock / 256 = 62.5 kHz carrier: ~28x above the 2200 Hz space
 * tone (room for a simple RC filter to reject it) while keeping 8-bit
 * (256-step) duty resolution for the sine waveform.
 */
#define TX_PWM_PERIOD_CYCLES 256
#define TX_PWM_IDLE_PULSE (TX_PWM_PERIOD_CYCLES / 2)

#if DT_NODE_HAS_STATUS(TX_PWM_NODE, okay)
static const struct device *const tx_pwm_dev = DEVICE_DT_GET(TX_PWM_NODE);
#endif

extern "C" int audio_tx_pwm_init(void)
{
#if DT_NODE_HAS_STATUS(TX_PWM_NODE, okay)
	if (!device_is_ready(tx_pwm_dev)) {
		printk("Warning: TX Audio PWM device not ready\n");
		return -ENODEV;
	}

	/* Idle at 50% duty (centre level / silence) until a packet starts. */
	pwm_set_cycles(tx_pwm_dev, TX_PWM_CHANNEL, TX_PWM_PERIOD_CYCLES, TX_PWM_IDLE_PULSE, 0);
	printk("TX Audio Pin 20 Hardware PWM DAC initialized (ch %d, %u Hz carrier)\n",
	       TX_PWM_CHANNEL, 16000000u / TX_PWM_PERIOD_CYCLES);
#else
	printk("Warning: pwm0 DT node not available\n");
#endif
	return 0;
}

static float g_tx_gain = 0.30f; /* Default 30% amplitude (~0.99V p-p) to prevent overdriving mic input */

extern "C" void audio_tx_set_gain(float gain)
{
	if (gain < 0.01f) gain = 0.01f;
	if (gain > 1.0f) gain = 1.0f;
	g_tx_gain = gain;
	printk("TX Audio Gain set to %d%% amplitude\n", (int)(g_tx_gain * 100.0f + 0.5f));
}

extern "C" float audio_tx_get_gain(void)
{
	return g_tx_gain;
}

static bool g_tx_fx25_enabled = false; /* Opt-in: adds RS(255,239) FEC, at the cost of fixed ~1.7s/frame airtime */

extern "C" void audio_tx_set_fx25(bool enable)
{
	g_tx_fx25_enabled = enable;
	printk("TX FX.25 FEC %s\n", enable ? "enabled" : "disabled");
}

extern "C" bool audio_tx_get_fx25(void)
{
	return g_tx_fx25_enabled;
}


extern "C" void audio_tx_pwm_play_packet(const uint8_t *data, size_t len, app_mode_t mode)
{
	if (data == NULL || len == 0) {
		return;
	}

	bool ptt_needed = (mode == APP_MODE_PACKET_TNC || mode == APP_MODE_DIGIPEATER);
	if (!ptt_needed) {
		return;
	}

	/* Every producer that queues a TX packet for this mode (ax25_encode(),
	 * or a KISS DATA payload straight off the wire) hands over a frame
	 * body with no FCS - it's a modem-only concept. AFSKModulator computes
	 * and appends the real one below, so `data`/`len` go in unmodified.
	 */

	printk("[AFSK TX] Modulating AX.25 frame (%d bytes) to 1200bps AFSK audio tones on Pin 20 (Gain: %.0f%%)...\n",
	       (int)len, (double)(g_tx_gain * 100.0f));

	AFSKModulator modulator(9600, 1200);

	std::vector<float> pcm_samples;
	if (g_tx_fx25_enabled) {
		pcm_samples = modulator.modulate_fx25_frame(data, len);
		if (pcm_samples.empty()) {
			printk("[AFSK TX] Frame too large for FX.25 RS(255,239) block - falling back to plain AX.25\n");
		} else {
			printk("[AFSK TX] Wrapped in FX.25 FEC (RS(255,239) block, 16 correctable bytes)\n");
		}
	}
	if (pcm_samples.empty()) {
		pcm_samples = modulator.modulate_frame(data, len);
	}

#if DT_NODE_HAS_STATUS(TX_PWM_NODE, okay)
	if (!device_is_ready(tx_pwm_dev)) {
		return;
	}

	printk("[AFSK TX] Playing %d PCM audio samples via 62.5kHz hardware PWM on Pin 20 (Pin 22 PTT Active-HIGH)\n",
	       (int)pcm_samples.size());

	/* 1. Key PTT (Pin 22 -> HIGH 3.3V) */
	ptt_set(true);

	/* Pre-TX Delay (50ms) */
	k_msleep(250);

	/* 2. Update the PWM duty cycle at the 9600 Hz sample rate; the
	 * hardware carries the actual carrier toggling autonomously between
	 * updates, so no per-carrier-edge CPU timing is needed.
	 *
	 * Schedule against the REAL hardware cycle counter (k_cycle_get_32(),
	 * 32.768kHz RTC on this board - ~30.5us quantization), re-read fresh
	 * every iteration, so any per-iteration overhead (pwm_set_cycles(),
	 * loop bookkeeping) self-corrects instead of silently accumulating
	 * as drift over a long frame - a prior version tracked only requested
	 * k_busy_wait() duration, which had no way to notice or compensate for
	 * that overhead and drifted worse the longer the frame ran. The actual
	 * pause still goes through k_busy_wait() (nrfx_coredep_delay_us(), true
	 * 64MHz-cycle precision) rather than a CPU-hogging spin on the coarse
	 * RTC counter.
	 */
	const uint32_t cycles_per_sec = sys_clock_hw_cycles_per_sec();
	const uint32_t start_cycle = k_cycle_get_32();

	for (size_t i = 0; i < pcm_samples.size(); i++) {
		uint32_t target_cycle = start_cycle + (uint32_t)((uint64_t)i * cycles_per_sec / 9600);
		int32_t cycles_remaining = (int32_t)(target_cycle - k_cycle_get_32());
		if (cycles_remaining > 0) {
			uint32_t wait_us = (uint32_t)((uint64_t)cycles_remaining * 1000000 / cycles_per_sec);
			k_busy_wait(wait_us);
		}

		float s = pcm_samples[i];
		/* Center-biased scaling with g_tx_gain attenuation to eliminate clipping/overdriving */
		float norm = 0.5f + (s * 0.5f * g_tx_gain);
		if (norm < 0.0f) norm = 0.0f;
		if (norm > 1.0f) norm = 1.0f;

		uint32_t pulse = (uint32_t)(norm * TX_PWM_PERIOD_CYCLES);

		pwm_set_cycles(tx_pwm_dev, TX_PWM_CHANNEL, TX_PWM_PERIOD_CYCLES, pulse, 0);
	}

	/* Ensure output settles back to centre level at end of audio stream */
	pwm_set_cycles(tx_pwm_dev, TX_PWM_CHANNEL, TX_PWM_PERIOD_CYCLES, TX_PWM_IDLE_PULSE, 0);

	/* Post-TX Delay (50ms) */
	k_msleep(250);

	/* 3. Unkey PTT (Pin 22 -> LOW 0V) */
	ptt_set(false);
#endif
}

extern "C" void audio_tx_pwm_test_tone(void)
{
#if DT_NODE_HAS_STATUS(TX_PWM_NODE, okay)
	if (!device_is_ready(tx_pwm_dev)) {
		return;
	}

	const uint32_t duration_ms = 3000;
	const uint32_t total_samples = 9600u * duration_ms / 1000u;
	const float phase_inc = 2.0f * 3.14159265f * 1200.0f / 9600.0f; /* 1200Hz mark tone */

	printk("[AFSK TX] TEST TONE: continuous 1200Hz for %ums on Pin 20 (Gain %d%%)\n",
	       duration_ms, (int)(g_tx_gain * 100.0f + 0.5f));

	ptt_set(true);
	k_msleep(50);

	float phase = 0.0f;
	const uint32_t cycles_per_sec = sys_clock_hw_cycles_per_sec();
	const uint32_t start_cycle = k_cycle_get_32();

	for (uint32_t i = 0; i < total_samples; i++) {
		uint32_t target_cycle = start_cycle + (uint32_t)((uint64_t)i * cycles_per_sec / 9600);
		int32_t cycles_remaining = (int32_t)(target_cycle - k_cycle_get_32());
		if (cycles_remaining > 0) {
			uint32_t wait_us = (uint32_t)((uint64_t)cycles_remaining * 1000000 / cycles_per_sec);
			k_busy_wait(wait_us);
		}

		float s = std::sin(phase);
		phase += phase_inc;
		if (phase >= 2.0f * 3.14159265f) {
			phase -= 2.0f * 3.14159265f;
		}

		float norm = 0.5f + (s * 0.5f * g_tx_gain);
		if (norm < 0.0f) norm = 0.0f;
		if (norm > 1.0f) norm = 1.0f;

		uint32_t pulse = (uint32_t)(norm * TX_PWM_PERIOD_CYCLES);
		pwm_set_cycles(tx_pwm_dev, TX_PWM_CHANNEL, TX_PWM_PERIOD_CYCLES, pulse, 0);
	}

	pwm_set_cycles(tx_pwm_dev, TX_PWM_CHANNEL, TX_PWM_PERIOD_CYCLES, TX_PWM_IDLE_PULSE, 0);
	k_msleep(50);
	ptt_set(false);

	printk("[AFSK TX] TEST TONE: done\n");
#endif
}
