"""
Unit Test Suite & Software Loopback Test for AFSK 1200bps Modulator/Demodulator Engine.

Phase 3 Requirements:
1. Core Modulator Class tests (continuous phase 1200Hz/2200Hz, NRZI, Pin 22 PTT timing).
2. Core Demodulator Class tests (Pin 31 filter, frequency discrimination, software DPLL).
3. Software Loopback Test (Verifying encoded text successfully passes through modulator -> demodulator).
"""

import pytest
import numpy as np
from afsk_dsp import AFSKModulator, AFSKDemodulator, crc16_hdlc


def test_crc16_hdlc():
    """Verify CRC-16 HDLC computation matches known test vector."""
    test_data = b"123456789"
    # CRC-16 HDLC for "123456789" is 0x906E
    calc = crc16_hdlc(test_data)
    assert calc == 0x906E, f"Expected 0x906E, got {hex(calc)}"


def test_modulator_nrzi_and_bit_stuffing():
    """Verify HDLC bit stuffing inserts 0 after 5 consecutive 1s."""
    mod = AFSKModulator()

    # 5 consecutive ones -> stuffed zero inserted
    bits = [1, 1, 1, 1, 1, 1]
    stuffed = mod.hdlc_bit_stuff(bits)
    assert stuffed == [1, 1, 1, 1, 1, 0, 1]

    # NRZI encoding test: 0 causes transition, 1 holds level
    mod._reset_state()
    nrzi = mod.nrzi_encode([0, 1, 0, 0])
    # initial level = 1
    # 0 -> 0
    # 1 -> 0
    # 0 -> 1
    # 0 -> 0
    assert nrzi == [0, 0, 1, 0]


def test_modulator_ptt_timing_and_pins():
    """Verify Pin 22 PTT timing metadata and constraints."""
    mod = AFSKModulator(sample_rate=9600, baud_rate=1200)

    # Pin constraints check
    assert mod.PTT_PIN == 22
    assert mod.TX_PIN == 20
    assert mod.RX_PIN == 17

    pcm, ptt_info = mod.modulate_frame(b"HELLO WORLD")

    assert ptt_info["ptt_pin"] == 22
    assert ptt_info["ptt_active"] is True
    assert ptt_info["total_samples"] == len(pcm)
    assert ptt_info["preamble_samples"] > 0
    assert ptt_info["postamble_samples"] > 0


def test_modulator_continuous_phase():
    """Verify continuous phase audio generation (no abrupt phase jumps)."""
    mod = AFSKModulator(sample_rate=9600, baud_rate=1200)
    pcm, _ = mod.modulate_frame(b"TEST PHASES")

    # The maximum first-difference step of a clean 2200Hz sine at 9600Hz sample rate is <= sin(2*pi*2200/9600) ~ 0.99
    # Phase discontinuities would produce huge delta spikes
    diffs = np.abs(np.diff(pcm))
    max_diff = np.max(diffs)
    # 2*pi*2200/9600 = 1.44 rad step -> max diff between adjacent samples is ~ 2*sin(1.44/2) = 1.30
    assert max_diff < 1.45, f"Phase discontinuity detected! max_diff={max_diff}"


def test_demodulator_dpll_and_filter():
    """Verify demodulator BPF and DPLL components."""
    demod = AFSKDemodulator(sample_rate=9600, baud_rate=1200)

    assert demod.RX_PIN == 17
    assert demod.PTT_PIN == 22

    # Generate synthetic 1200Hz sine wave
    t = np.arange(9600) / 9600.0
    sine_1200 = np.sin(2.0 * np.pi * 1200.0 * t).astype(np.float32)

    filtered = demod.filter_rx_audio(sine_1200)
    assert len(filtered) == len(sine_1200)

    baseband = demod.discriminate_frequency(filtered)
    assert len(baseband) == len(filtered)
    # 1200Hz should produce positive baseband output
    assert np.mean(baseband[500:]) > 0.0


@pytest.mark.parametrize(
    "text_payload",
    [
        b"N0CALL>APRS,WIDE1-1:Hello AFSK World!",
        b"TEST PACKET 12345",
        b"Beacon: Lat 37.7749 Long -122.4194",
    ],
)
def test_software_loopback_clean(text_payload):
    """
    Complete Software Loopback Test:
    Encodes text -> Modulates continuous phase audio -> Demodulates audio -> Verifies exact text payload.
    """
    mod = AFSKModulator(sample_rate=9600, baud_rate=1200)
    demod = AFSKDemodulator(sample_rate=9600, baud_rate=1200)

    # 1. Modulate Frame to PCM Audio
    pcm_audio, ptt_info = mod.modulate_frame(text_payload)
    assert ptt_info["ptt_pin"] == 22

    # 2. Pass PCM Audio through Demodulator (Pin 31 Input)
    rx_frames = demod.process_rx_audio(pcm_audio)

    # 3. Assert Decoded Payload Matches Input Text
    assert len(rx_frames) >= 1, "Demodulator failed to recover frame!"
    assert rx_frames[0] == text_payload, f"Decoded payload '{rx_frames[0]}' does not match expected '{text_payload}'"


def test_software_loopback_with_noise():
    """Verify software loopback under moderate additive Gaussian noise (AWGN)."""
    text_payload = b"NOISY CHANNEL TEST"

    mod = AFSKModulator(sample_rate=9600, baud_rate=1200)
    demod = AFSKDemodulator(sample_rate=9600, baud_rate=1200)

    pcm_audio, _ = mod.modulate_frame(text_payload)

    # Add Gaussian noise (SNR ~ 15dB)
    np.random.seed(42)
    noise = np.random.normal(0, 0.1, size=len(pcm_audio)).astype(np.float32)
    noisy_audio = pcm_audio + noise

    rx_frames = demod.process_rx_audio(noisy_audio)

    assert len(rx_frames) >= 1, "Failed to decode in noisy channel"
    assert rx_frames[0] == text_payload
