#include "afsk_modulator.hpp"
#include "afsk_demodulator.hpp"
#include <iostream>
#include <string>
#include <cassert>
#include <vector>
#include <random>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "❌ [FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl << std::flush; \
            return false; \
        } \
    } while(0)

#define RUN_TEST(test_func) \
    do { \
        std::cout << "Running " << #test_func << "... " << std::flush; \
        if (test_func()) { \
            std::cout << "✅ PASSED" << std::endl << std::flush; \
        } else { \
            std::cout << "❌ FAILED" << std::endl << std::flush; \
            failures++; \
        } \
        total++; \
    } while(0)

static bool test_hardware_pin_constraints() {
    AFSKModulator mod;
    AFSKDemodulator demod;

    TEST_ASSERT(mod.get_ptt_pin() != 9 && mod.get_ptt_pin() != 10, "Modulator must not touch Pin 9 or 10");
    TEST_ASSERT(mod.get_tx_pin() != 9 && mod.get_tx_pin() != 10, "Modulator must not touch Pin 9 or 10");
    TEST_ASSERT(demod.get_rx_pin() != 9 && demod.get_rx_pin() != 10, "Demodulator must not touch Pin 9 or 10");

    TEST_ASSERT(demod.get_rx_pin() == 31, "RX Audio must be mapped to Pin 31");
    TEST_ASSERT(mod.get_tx_pin() == 20, "TX Audio must be mapped to Pin 20");
    TEST_ASSERT(mod.get_ptt_pin() == 22, "PTT Control must be mapped to Pin 22");

    return true;
}

static bool test_crc16_hdlc() {
    const uint8_t test_data[] = "123456789";
    uint16_t crc = AFSKModulator::calculate_crc16(test_data, 9);
    TEST_ASSERT(crc == 0x906E, "CRC16 computation failure");
    return true;
}

static bool test_modulator_hdlc_stuffing() {
    std::vector<uint8_t> bits = {1, 1, 1, 1, 1, 1};
    std::vector<uint8_t> stuffed = AFSKModulator::hdlc_bit_stuff(bits);
    std::vector<uint8_t> expected = {1, 1, 1, 1, 1, 0, 1};
    TEST_ASSERT(stuffed == expected, "HDLC bit stuffing failure");
    return true;
}

static bool test_modulator_ptt_timing() {
    AFSKModulator mod(9600, 1200);
    const uint8_t payload[] = "TEST PACKET PTT";
    uint32_t preamble_samples = 0;
    uint32_t postamble_samples = 0;

    std::vector<float> pcm = mod.modulate_frame(payload, sizeof(payload) - 1, &preamble_samples, &postamble_samples);

    TEST_ASSERT(!pcm.empty(), "Modulator failed to produce PCM");
    TEST_ASSERT(preamble_samples > 0, "Preamble samples must be > 0");
    TEST_ASSERT(postamble_samples > 0, "Postamble samples must be > 0");
    TEST_ASSERT(mod.get_ptt_pin() == 22, "PTT Pin must be 22");

    return true;
}

static bool test_software_loopback_clean() {
    AFSKModulator mod(9600, 1200);
    AFSKDemodulator demod(9600, 1200);

    std::string test_str = "N0CALL>APRS,WIDE1-1:Hello AFSK 1200bps World!";
    const uint8_t* payload = reinterpret_cast<const uint8_t*>(test_str.data());
    size_t payload_len = test_str.size();

    std::vector<float> pcm = mod.modulate_frame(payload, payload_len);

    std::vector<std::vector<uint8_t>> decoded_frames = demod.process_buffer(pcm.data(), pcm.size());

    TEST_ASSERT(decoded_frames.size() >= 1, "Demodulator failed to decode frame in loopback");

    std::string decoded_str(decoded_frames[0].begin(), decoded_frames[0].end());
    TEST_ASSERT(decoded_str == test_str, "Decoded text payload mismatch");

    return true;
}

static bool test_software_loopback_noisy() {
    AFSKModulator mod(9600, 1200);
    AFSKDemodulator demod(9600, 1200);

    std::string test_str = "BEACON: Lat 37.7749 Long -122.4194";
    const uint8_t* payload = reinterpret_cast<const uint8_t*>(test_str.data());
    size_t payload_len = test_str.size();

    std::vector<float> pcm = mod.modulate_frame(payload, payload_len);

    std::mt19937 rng(42);
    std::normal_distribution<float> dist(0.0f, 0.05f);
    for (float& sample : pcm) {
        sample += dist(rng);
    }

    std::vector<std::vector<uint8_t>> decoded_frames = demod.process_buffer(pcm.data(), pcm.size());

    TEST_ASSERT(decoded_frames.size() >= 1, "Demodulator failed in noisy loopback");

    std::string decoded_str(decoded_frames[0].begin(), decoded_frames[0].end());
    TEST_ASSERT(decoded_str == test_str, "Decoded payload mismatch under noise");

    return true;
}

// Reed-Solomon correction limits themselves are covered exhaustively at
// the block level by test_fx25.c (exact byte-error math, up to and past
// RS(255,239)'s 8-byte capacity). This test instead covers what that one
// can't: that the correlation-tag search, block capture, and RS decode
// are wired correctly into the real sample-by-sample AFSK pipeline.
static bool test_fx25_loopback_clean() {
    AFSKModulator mod(9600, 1200);
    AFSKDemodulator demod(9600, 1200);

    std::string test_str = "N0CALL>APRS,WIDE1-1:FX.25 over-the-air test";
    const uint8_t* payload = reinterpret_cast<const uint8_t*>(test_str.data());
    size_t payload_len = test_str.size();

    std::vector<float> pcm = mod.modulate_fx25_frame(payload, payload_len);
    TEST_ASSERT(!pcm.empty(), "FX.25 modulator failed to produce PCM");

    std::vector<std::vector<uint8_t>> decoded_frames = demod.process_buffer(pcm.data(), pcm.size());
    TEST_ASSERT(decoded_frames.size() >= 1, "FX.25 demodulator failed to decode frame via correlation tag + RS block");

    std::string decoded_str(decoded_frames[0].begin(), decoded_frames[0].end());
    TEST_ASSERT(decoded_str == test_str, "FX.25 decoded text payload mismatch");
    TEST_ASSERT(demod.last_fx25_corrections() == 0, "Clean FX.25 frame should decode with 0 corrections");

    return true;
}

int main() {
    int total = 0;
    int failures = 0;

    std::cout << "==========================================================" << std::endl << std::flush;
    std::cout << " AFSK 1200bps Modulator/Demodulator Unit Test Suite (Phase 3)" << std::endl << std::flush;
    std::cout << "==========================================================" << std::endl << std::flush;

    RUN_TEST(test_hardware_pin_constraints);
    RUN_TEST(test_crc16_hdlc);
    RUN_TEST(test_modulator_hdlc_stuffing);
    RUN_TEST(test_modulator_ptt_timing);
    RUN_TEST(test_software_loopback_clean);
    RUN_TEST(test_software_loopback_noisy);
    RUN_TEST(test_fx25_loopback_clean);

    std::cout << "==========================================================" << std::endl << std::flush;
    if (failures == 0) {
        std::cout << "🎉 ALL " << total << " UNIT TESTS PASSED SUCCESSFULLY!" << std::endl << std::flush;
        return 0;
    } else {
        std::cout << "❌ " << failures << " OF " << total << " TESTS FAILED." << std::endl << std::flush;
        return 1;
    }
}
