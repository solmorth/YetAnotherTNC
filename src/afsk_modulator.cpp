#include "afsk_modulator.hpp"
#include "fx25.h"
#include <cmath>
#include <vector>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

AFSKModulator::AFSKModulator(uint32_t sample_rate, uint32_t baud_rate,
                             float mark_freq, float space_freq,
                             uint16_t tx_delay_flags, uint16_t tx_tail_flags)
    : sample_rate_(sample_rate), baud_rate_(baud_rate),
      mark_freq_(mark_freq), space_freq_(space_freq),
      tx_delay_flags_(tx_delay_flags), tx_tail_flags_(tx_tail_flags),
      phase_(0.0f), current_nrzi_level_(1)
{
    samples_per_symbol_ = static_cast<float>(sample_rate_) / static_cast<float>(baud_rate_);
}

void AFSKModulator::reset_state() {
    phase_ = 0.0f;
    current_nrzi_level_ = 1;
}

uint16_t AFSKModulator::calculate_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0x8408;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFF;
}

std::vector<uint8_t> AFSKModulator::bytes_to_bits(const uint8_t* data, size_t len) {
    std::vector<uint8_t> bits;
    bits.reserve(len * 8);
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
            bits.push_back((b >> bit_idx) & 1);
        }
    }
    return bits;
}

std::vector<uint8_t> AFSKModulator::hdlc_bit_stuff(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> stuffed;
    stuffed.reserve(bits.size() + bits.size() / 5);
    int ones_count = 0;

    for (uint8_t bit : bits) {
        stuffed.push_back(bit);
        if (bit == 1) {
            ones_count++;
            if (ones_count == 5) {
                stuffed.push_back(0); // Insert stuffed zero
                ones_count = 0;
            }
        } else {
            ones_count = 0;
        }
    }
    return stuffed;
}

std::vector<uint8_t> AFSKModulator::nrzi_encode(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> nrzi;
    nrzi.reserve(bits.size());
    uint8_t level = current_nrzi_level_;

    for (uint8_t bit : bits) {
        if (bit == 0) {
            level = 1 - level; // Toggle
        }
        nrzi.push_back(level);
    }
    current_nrzi_level_ = level;
    return nrzi;
}

std::vector<float> AFSKModulator::generate_audio_pcm(const std::vector<uint8_t>& nrzi_levels) {
    std::vector<float> pcm;
    pcm.reserve(static_cast<size_t>(nrzi_levels.size() * samples_per_symbol_));

    const uint32_t n_samples = static_cast<uint32_t>(samples_per_symbol_ + 0.5f);

    for (uint8_t level : nrzi_levels) {
        float freq = (level == 1) ? mark_freq_ : space_freq_;
        float phase_inc = 2.0f * static_cast<float>(M_PI) * freq / static_cast<float>(sample_rate_);

        for (uint32_t s = 0; s < n_samples; s++) {
            pcm.push_back(std::sin(phase_));
            phase_ += phase_inc;
            if (phase_ >= 2.0f * static_cast<float>(M_PI)) {
                phase_ -= 2.0f * static_cast<float>(M_PI);
            }
        }
    }
    return pcm;
}

std::vector<float> AFSKModulator::modulate_frame(const uint8_t* payload, size_t payload_len,
                                                  uint32_t* out_preamble_samples,
                                                  uint32_t* out_postamble_samples) {
    reset_state();

    // 1. Calculate CRC-16
    uint16_t crc = calculate_crc16(payload, payload_len);
    std::vector<uint8_t> frame_bytes(payload, payload + payload_len);
    frame_bytes.push_back(crc & 0xFF);
    frame_bytes.push_back((crc >> 8) & 0xFF);

    // 2. Preamble (0x7E flags)
    std::vector<uint8_t> preamble_bytes(tx_delay_flags_, 0x7E);
    std::vector<uint8_t> preamble_bits = bytes_to_bits(preamble_bytes.data(), preamble_bytes.size());

    // 3. Payload + CRC (bit-stuffed)
    std::vector<uint8_t> payload_bits = bytes_to_bits(frame_bytes.data(), frame_bytes.size());
    std::vector<uint8_t> stuffed_payload_bits = hdlc_bit_stuff(payload_bits);

    // 4. Postamble (0x7E flags)
    std::vector<uint8_t> postamble_bytes(tx_tail_flags_, 0x7E);
    std::vector<uint8_t> postamble_bits = bytes_to_bits(postamble_bytes.data(), postamble_bytes.size());

    // Combine all bits
    std::vector<uint8_t> all_bits;
    all_bits.reserve(preamble_bits.size() + stuffed_payload_bits.size() + postamble_bits.size());
    all_bits.insert(all_bits.end(), preamble_bits.begin(), preamble_bits.end());
    all_bits.insert(all_bits.end(), stuffed_payload_bits.begin(), stuffed_payload_bits.end());
    all_bits.insert(all_bits.end(), postamble_bits.begin(), postamble_bits.end());

    // NRZI & PCM generation
    std::vector<uint8_t> nrzi_levels = nrzi_encode(all_bits);

    if (out_preamble_samples) {
        *out_preamble_samples = static_cast<uint32_t>(preamble_bits.size() * samples_per_symbol_ + 0.5f);
    }
    if (out_postamble_samples) {
        *out_postamble_samples = static_cast<uint32_t>(postamble_bits.size() * samples_per_symbol_ + 0.5f);
    }

    return generate_audio_pcm(nrzi_levels);
}

std::vector<float> AFSKModulator::modulate_fx25_frame(const uint8_t* payload, size_t payload_len) {
    reset_state();

    uint8_t block[FX25_BLOCK_SIZE];
    if (fx25_build_block(payload, payload_len, block) != 0) {
        return {}; // Doesn't fit a single RS(255,239) block - fall back to plain AX.25.
    }

    uint8_t tag_bytes[FX25_TAG_BYTES];
    uint64_t tag = FX25_TAG_01;
    for (int i = 0; i < FX25_TAG_BYTES; i++) {
        tag_bytes[i] = (uint8_t)(tag >> (i * 8));
    }

    std::vector<uint8_t> preamble_bytes(tx_delay_flags_, 0x7E);
    std::vector<uint8_t> postamble_bytes(tx_tail_flags_, 0x7E);

    // Tag + RS block are NOT HDLC bit-stuffed (fx25_build_block already
    // baked the flags/stuffing into the block); only the ordinary
    // preamble/postamble flags surround them, same as a plain AX.25 frame.
    std::vector<uint8_t> all_bits = bytes_to_bits(preamble_bytes.data(), preamble_bytes.size());
    std::vector<uint8_t> tag_bits = bytes_to_bits(tag_bytes, sizeof(tag_bytes));
    std::vector<uint8_t> block_bits = bytes_to_bits(block, sizeof(block));
    std::vector<uint8_t> postamble_bits = bytes_to_bits(postamble_bytes.data(), postamble_bytes.size());

    all_bits.insert(all_bits.end(), tag_bits.begin(), tag_bits.end());
    all_bits.insert(all_bits.end(), block_bits.begin(), block_bits.end());
    all_bits.insert(all_bits.end(), postamble_bits.begin(), postamble_bits.end());

    std::vector<uint8_t> nrzi_levels = nrzi_encode(all_bits);
    return generate_audio_pcm(nrzi_levels);
}
