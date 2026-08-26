#ifndef AFSK_MODULATOR_HPP
#define AFSK_MODULATOR_HPP

#include <cstdint>
#include <cstddef>
#include <vector>

/**
 * Hardware Pin Constraints:
 * - Pin 9 & Pin 10: Reserved for Serial0 UART (NEVER touched or reconfigured).
 * - Pin 31: RX Audio Input (ADC, AIN7)
 * - Pin 20: TX Audio Output
 * - Pin 22: PTT Control Output
 */
constexpr uint8_t HARDWARE_PIN_RX_AUDIO = 31;
constexpr uint8_t HARDWARE_PIN_TX_AUDIO = 20;
constexpr uint8_t HARDWARE_PIN_PTT      = 22;

class AFSKModulator {
public:
    AFSKModulator(uint32_t sample_rate = 9600, uint32_t baud_rate = 1200,
                  float mark_freq = 1200.0f, float space_freq = 2200.0f,
                  uint16_t tx_delay_flags = 16, uint16_t tx_tail_flags = 8);

    uint8_t get_ptt_pin() const { return HARDWARE_PIN_PTT; }
    uint8_t get_tx_pin() const  { return HARDWARE_PIN_TX_AUDIO; }

    /**
     * Compute 16-bit HDLC/AX.25 CRC-16 (FCS).
     */
    static uint16_t calculate_crc16(const uint8_t* data, size_t len);

    /**
     * Convert bytes to LSB-first bit stream.
     */
    static std::vector<uint8_t> bytes_to_bits(const uint8_t* data, size_t len);

    /**
     * Perform HDLC bit-stuffing (insert 0 after 5 consecutive 1s).
     */
    static std::vector<uint8_t> hdlc_bit_stuff(const std::vector<uint8_t>& bits);

    /**
     * Modulate a payload frame into continuous-phase PCM float audio samples.
     * Manages Pin 22 PTT timing metadata.
     */
    std::vector<float> modulate_frame(const uint8_t* payload, size_t payload_len,
                                      uint32_t* out_preamble_samples = nullptr,
                                      uint32_t* out_postamble_samples = nullptr);

    /**
     * Modulate an AX.25 frame wrapped in FX.25 forward error correction
     * (correlation tag + RS(255,239) block instead of a plain HDLC frame).
     * Returns an empty vector if the frame doesn't fit in one RS block -
     * caller should fall back to modulate_frame() for plain AX.25.
     */
    std::vector<float> modulate_fx25_frame(const uint8_t* payload, size_t payload_len);

private:
    uint32_t sample_rate_;
    uint32_t baud_rate_;
    float mark_freq_;
    float space_freq_;
    float samples_per_symbol_;
    uint16_t tx_delay_flags_;
    uint16_t tx_tail_flags_;

    float phase_;
    uint8_t current_nrzi_level_;

    void reset_state();
    std::vector<uint8_t> nrzi_encode(const std::vector<uint8_t>& bits);
    std::vector<float> generate_audio_pcm(const std::vector<uint8_t>& nrzi_levels);
};

#endif // AFSK_MODULATOR_HPP
