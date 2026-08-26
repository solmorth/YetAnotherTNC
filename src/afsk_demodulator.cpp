#include "afsk_demodulator.hpp"
#include "afsk_modulator.hpp"
#include "ax25.h"
#include <cmath>
#include <cstring>
#include <iostream>

/* rx_bit_buffer_ has no natural cap otherwise: a noisy/open-squelch run
 * that never produces a closing 0x7E flag or 7 consecutive 1-bits would
 * grow it forever, exhausting the heap. Cap it at the largest frame this
 * codebase ever produces (see AX25_MAX_FRAME_LEN) and abort the frame if
 * exceeded, same as kiss_decode_byte's overflow handling.
 */
static constexpr size_t MAX_RX_FRAME_BITS = AX25_MAX_FRAME_LEN * 8;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

BiquadFilter::BiquadFilter() : b0(1), b1(0), b2(0), a1(0), a2(0), x1(0), x2(0), y1(0), y2(0) {}

void BiquadFilter::reset() {
    x1 = x2 = y1 = y2 = 0.0f;
}

float BiquadFilter::process(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

BiquadFilter BiquadFilter::lowpass(float f0, float fs, float q) {
    BiquadFilter f;
    float w0 = 2.0f * static_cast<float>(M_PI) * f0 / fs;
    float alpha = std::sin(w0) / (2.0f * q);
    float cos_w0 = std::cos(w0);

    float b0 = (1.0f - cos_w0) / 2.0f;
    float b1 = 1.0f - cos_w0;
    float b2 = (1.0f - cos_w0) / 2.0f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cos_w0;
    float a2 = 1.0f - alpha;

    f.b0 = b0 / a0;
    f.b1 = b1 / a0;
    f.b2 = b2 / a0;
    f.a1 = a1 / a0;
    f.a2 = a2 / a0;
    return f;
}

BiquadFilter BiquadFilter::bandpass(float f0, float fs, float q) {
    BiquadFilter f;
    float w0 = 2.0f * static_cast<float>(M_PI) * f0 / fs;
    float alpha = std::sin(w0) / (2.0f * q);
    float cos_w0 = std::cos(w0);

    float b0 = alpha;
    float b1 = 0.0f;
    float b2 = -alpha;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cos_w0;
    float a2 = 1.0f - alpha;

    f.b0 = b0 / a0;
    f.b1 = b1 / a0;
    f.b2 = b2 / a0;
    f.a1 = a1 / a0;
    f.a2 = a2 / a0;
    return f;
}

AFSKDemodulator::AFSKDemodulator(uint32_t sample_rate, uint32_t baud_rate,
                                 float mark_freq, float space_freq)
    : sample_rate_(sample_rate), baud_rate_(baud_rate)
{
    samples_per_symbol_ = static_cast<float>(sample_rate_) / static_cast<float>(baud_rate_);

    float mark_dc = 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * mark_freq / static_cast<float>(sample_rate_));
    float space_dc = 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * space_freq / static_cast<float>(sample_rate_));
    discrim_threshold_ = (mark_dc + space_dc) / 2.0f;

    bpf_input_ = BiquadFilter::bandpass(1700.0f, static_cast<float>(sample_rate_), 0.7071f);
    lpf_baseband_ = BiquadFilter::lowpass(1200.0f, static_cast<float>(sample_rate_), 0.7071f);

    reset();
}

void AFSKDemodulator::reset() {
    bpf_input_.reset();
    lpf_baseband_.reset();
    prev_filtered_sample_ = 0.0f;
    dpll_counter_ = 0;
    prev_baseband_sign_ = true;
    prev_sampled_level_ = 1;

    bit_shift_reg_ = 0;
    ones_count_ = 0;
    in_frame_ = false;
    rx_bit_buffer_.clear();

    fx25_tag_shift_ = 0;
    fx25_capturing_ = false;
    fx25_bit_count_ = 0;
    fx25_last_corrections_ = -1;
}

bool AFSKDemodulator::process_sample(float sample, std::vector<uint8_t>& out_payload) {
    // 1. Frequency Discrimination (1-sample delay product)
    float raw_product = sample * prev_filtered_sample_;
    prev_filtered_sample_ = sample;

    // 2. Lowpass filter baseband output
    float baseband_raw = lpf_baseband_.process(raw_product);

    // Fixed threshold (theoretical midpoint between mark/space discriminator
    // output, computed once in the constructor). An adaptive DC tracker was
    // here before, but it erodes the discrimination margin over long
    // uninterrupted same-tone runs - harmless for plain AX.25 (HDLC bit
    // stuffing caps runs at 5 bits) but FX.25's unstuffed RS parity bytes
    // have no such cap, and the tracker would converge toward the held
    // tone's own level, occasionally flipping marginal bit decisions and
    // corrupting the correlation-tag search.
    float baseband = baseband_raw - discrim_threshold_;

    bool curr_sign = (baseband >= 0.0f);

    // 4. Integer 8-state DPLL clock recovery
    if (curr_sign != prev_baseband_sign_) {
        if (dpll_counter_ > 0 && dpll_counter_ <= 3) {
            dpll_counter_--;
        } else if (dpll_counter_ >= 5 && dpll_counter_ <= 7) {
            dpll_counter_++;
        }
        prev_baseband_sign_ = curr_sign;
    }

    bool packet_decoded = false;

    // Mid-symbol strobe
    if (dpll_counter_ == 4) {
        uint8_t curr_level = curr_sign ? 1 : 0;
        uint8_t nrzi_bit = (curr_level != prev_sampled_level_) ? 0 : 1;
        prev_sampled_level_ = curr_level;

        packet_decoded = process_afsk_bit(nrzi_bit, out_payload);
    }

    dpll_counter_++;
    if (dpll_counter_ >= 8) {
        dpll_counter_ = 0;
    }

    return packet_decoded;
}

bool AFSKDemodulator::process_demodulated_bit(uint8_t bit, std::vector<uint8_t>& out_payload) {
    bit_shift_reg_ = ((bit_shift_reg_ << 1) | bit) & 0xFF;

    // HDLC Flag (0x7E)
    if (bit_shift_reg_ == 0x7E) {
        bool valid_frame = false;
        if (in_frame_ && rx_bit_buffer_.size() >= 7 + 16) {
            rx_bit_buffer_.resize(rx_bit_buffer_.size() - 7); // Remove 7 bits of the closing 0x7E flag

            std::vector<uint8_t> frame_bytes;
            for (size_t i = 0; i + 7 < rx_bit_buffer_.size(); i += 8) {
                uint8_t val = 0;
                for (int b = 0; b < 8; b++) {
                    if (rx_bit_buffer_[i + b]) {
                        val |= (1 << b);
                    }
                }
                frame_bytes.push_back(val);
            }

            if (frame_bytes.size() >= 3) {
                out_payload.assign(frame_bytes.begin(), frame_bytes.end() - 2);
                uint16_t rx_fcs = frame_bytes[frame_bytes.size() - 2] | (frame_bytes[frame_bytes.size() - 1] << 8);
                uint16_t calc_fcs = AFSKModulator::calculate_crc16(out_payload.data(), out_payload.size());

                if (rx_fcs == calc_fcs) {
                    valid_frame = true;
                }
            }
        }

        in_frame_ = true;
        rx_bit_buffer_.clear();
        ones_count_ = 0;
        return valid_frame;
    }

    if (in_frame_) {
        if (bit == 1) {
            ones_count_++;
            if (ones_count_ >= 7) {
                // Abort frame (7 or more consecutive 1s)
                in_frame_ = false;
                rx_bit_buffer_.clear();
                ones_count_ = 0;
                return false;
            }
            rx_bit_buffer_.push_back(1);
        } else {
            if (ones_count_ == 5) {
                // Stuffed zero! Discard bit
                ones_count_ = 0;
                return false;
            }
            ones_count_ = 0;
            rx_bit_buffer_.push_back(0);
        }

        if (rx_bit_buffer_.size() > MAX_RX_FRAME_BITS) {
            // Runaway frame: no flag or abort condition seen in longer than
            // the largest legal frame - noise, not a real packet. Drop it
            // and shed the oversized allocation instead of growing forever.
            in_frame_ = false;
            rx_bit_buffer_.clear();
            rx_bit_buffer_.shrink_to_fit();
            ones_count_ = 0;
            return false;
        }
    }

    return false;
}

bool AFSKDemodulator::process_afsk_bit(uint8_t bit, std::vector<uint8_t>& out_payload) {
    if (fx25_capturing_) {
        size_t byte_idx = fx25_bit_count_ >> 3;
        size_t bit_idx = fx25_bit_count_ & 7;
        if (bit) {
            fx25_block_[byte_idx] |= (uint8_t)(1u << bit_idx);
        }
        fx25_bit_count_++;

        if (fx25_bit_count_ < (size_t)FX25_BLOCK_SIZE * 8) {
            return false;
        }

        // Full RS(255,239) block captured - correct it and pull out the
        // AX.25 frame it carries.
        fx25_capturing_ = false;
        bit_shift_reg_ = 0;
        ones_count_ = 0;
        in_frame_ = false;
        rx_bit_buffer_.clear();

        out_payload.assign(FX25_DATA_SIZE, 0);
        size_t decoded_len = 0;
        int corrected = fx25_decode_block(fx25_block_, out_payload.data(), out_payload.size(), &decoded_len);
        if (corrected < 0) {
            out_payload.clear();
            return false;
        }
        out_payload.resize(decoded_len);
        fx25_last_corrections_ = corrected;
        return true;
    }

    // Correlation-tag search runs alongside the plain-AX.25 flag search
    // below; a match means an FX.25 block is starting on the air.
    fx25_tag_shift_ = (fx25_tag_shift_ >> 1) | ((uint64_t)bit << 63);
    if (__builtin_popcountll(fx25_tag_shift_ ^ (uint64_t)FX25_TAG_01) <= FX25_TAG_MAX_HAMMING) {
        fx25_capturing_ = true;
        fx25_bit_count_ = 0;
        memset(fx25_block_, 0, sizeof(fx25_block_));
        bit_shift_reg_ = 0;
        ones_count_ = 0;
        in_frame_ = false;
        rx_bit_buffer_.clear();
        return false;
    }

    // Only touch fx25_last_corrections_ when a frame actually completes
    // here - not on every idle/preamble bit - otherwise trailing
    // postamble flag bits processed after a successful FX.25 decode (in
    // this same process_buffer call) stomp the flag back to -1 before
    // the caller ever reads it.
    bool decoded = process_demodulated_bit(bit, out_payload);
    if (decoded) {
        fx25_last_corrections_ = -1;
    }
    return decoded;
}

std::vector<std::vector<uint8_t>> AFSKDemodulator::process_buffer(const float* samples, size_t count) {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> payload;

    for (size_t i = 0; i < count; i++) {
        if (process_sample(samples[i], payload)) {
            frames.push_back(payload);
            payload.clear();
        }
    }
    return frames;
}
