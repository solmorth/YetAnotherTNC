#ifndef AFSK_DEMODULATOR_HPP
#define AFSK_DEMODULATOR_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include "fx25.h"

/**
 * Biquad Filter (Direct Form I / II)
 */
struct BiquadFilter {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2;
    float y1, y2;

    BiquadFilter();
    void reset();
    float process(float x);

    static BiquadFilter lowpass(float f0, float fs, float q = 0.7071f);
    static BiquadFilter bandpass(float f0, float fs, float q = 1.0f);
};

class AFSKDemodulator {
public:
    AFSKDemodulator(uint32_t sample_rate = 9600, uint32_t baud_rate = 1200,
                    float mark_freq = 1200.0f, float space_freq = 2200.0f);

    uint8_t get_rx_pin() const { return 31; } // Pin 31 Input (ADC, AIN7)

    void reset();

    /**
     * Process a single audio sample (from Pin 31).
     * Returns true if a valid HDLC frame was decoded, outputting payload bytes.
     */
    bool process_sample(float sample, std::vector<uint8_t>& out_payload);

    /**
     * Process a buffer of audio samples.
     * Returns all decoded valid HDLC payload frames.
     */
    std::vector<std::vector<uint8_t>> process_buffer(const float* samples, size_t count);

    /**
     * Number of byte errors the last successfully decoded frame required
     * FX.25 Reed-Solomon correction for, or -1 if the last decoded frame
     * was plain AX.25 (no FX.25 correlation tag involved).
     */
    int last_fx25_corrections() const { return fx25_last_corrections_; }

private:
    uint32_t sample_rate_;
    uint32_t baud_rate_;
    float samples_per_symbol_;
    float discrim_threshold_;

    BiquadFilter bpf_input_;
    BiquadFilter lpf_baseband_;

    float prev_filtered_sample_;
    int dpll_counter_;
    bool prev_baseband_sign_;
    uint8_t prev_sampled_level_;

    // HDLC Framer state
    uint8_t bit_shift_reg_;
    int ones_count_;
    bool in_frame_;
    std::vector<uint8_t> rx_bit_buffer_;

    bool process_demodulated_bit(uint8_t bit, std::vector<uint8_t>& out_payload);

    // FX.25 correlation-tag search + RS(255,239) block capture, running
    // in parallel with the plain-AX.25 flag search above.
    bool process_afsk_bit(uint8_t bit, std::vector<uint8_t>& out_payload);
    uint64_t fx25_tag_shift_;
    bool fx25_capturing_;
    size_t fx25_bit_count_;
    uint8_t fx25_block_[FX25_BLOCK_SIZE];
    int fx25_last_corrections_;
};

#endif // AFSK_DEMODULATOR_HPP
