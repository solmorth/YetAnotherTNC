"""
AFSK 1200bps Modulator / Demodulator DSP Engine (Python)

Target Environment Details:
- Hardware Constraint 1: NEVER touch, reconfigure, or initialize Pins 9 & 10 (reserved for Serial0).
- Hardware Constraint 2: RX audio comes in on Pin 31 (ADC, AIN7), TX audio goes out on
  Pin 20, PTT on Pin 22.
"""

import numpy as np
import scipy.signal as signal
from typing import List, Tuple, Optional, Dict


def crc16_hdlc(data: bytes) -> int:
    """Calculate 16-bit HDLC/AX.25 FCS (CRC-16-CCITT inverted, LSB first)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
    return crc ^ 0xFFFF


class BiquadFilter:
    """2nd Order Direct Form I IIR Biquad Filter."""

    def __init__(self, b0, b1, b2, a1, a2):
        self.b0 = b0
        self.b1 = b1
        self.b2 = b2
        self.a1 = a1
        self.a2 = a2
        self.x1 = 0.0
        self.x2 = 0.0
        self.y1 = 0.0
        self.y2 = 0.0

    @classmethod
    def lowpass(cls, f0: float, fs: float, q: float = 0.7071):
        w0 = 2.0 * np.pi * f0 / fs
        alpha = np.sin(w0) / (2.0 * q)
        cos_w0 = np.cos(w0)
        b0 = (1.0 - cos_w0) / 2.0
        b1 = 1.0 - cos_w0
        b2 = (1.0 - cos_w0) / 2.0
        a0 = 1.0 + alpha
        a1 = -2.0 * cos_w0
        a2 = 1.0 - alpha
        return cls(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)

    @classmethod
    def bandpass(cls, f0: float, fs: float, q: float = 1.0):
        w0 = 2.0 * np.pi * f0 / fs
        alpha = np.sin(w0) / (2.0 * q)
        cos_w0 = np.cos(w0)
        b0 = alpha
        b1 = 0.0
        b2 = -alpha
        a0 = 1.0 + alpha
        a1 = -2.0 * cos_w0
        a2 = 1.0 - alpha
        return cls(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)

    def process_sample(self, x: float) -> float:
        y = self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2 - self.a1 * self.y1 - self.a2 * self.y2
        self.x2 = self.x1
        self.x1 = x
        self.y2 = self.y1
        self.y1 = y
        return y


class AFSKModulator:
    """
    Continuous-Phase AFSK 1200bps Modulator with NRZI encoding, HDLC bit-stuffing,
    and Pin 22 PTT timing logic.
    """

    RX_PIN = 17
    TX_PIN = 20
    PTT_PIN = 22  # Pin 22 dedicated for Push-To-Talk control

    def __init__(
        self,
        sample_rate: int = 9600,
        baud_rate: int = 1200,
        mark_freq: float = 1200.0,
        space_freq: float = 2200.0,
        tx_delay_flags: int = 16,
        tx_tail_flags: int = 8,
    ):
        self.sample_rate = sample_rate
        self.baud_rate = baud_rate
        self.mark_freq = mark_freq
        self.space_freq = space_freq
        self.samples_per_symbol = int(round(sample_rate / baud_rate))
        self.tx_delay_flags = tx_delay_flags
        self.tx_tail_flags = tx_tail_flags

        # Modulator state
        self.phase = 0.0
        self.current_nrzi_level = 1  # 1 = Mark (1200Hz), 0 = Space (2200Hz)

    def _reset_state(self):
        self.phase = 0.0
        self.current_nrzi_level = 1

    @staticmethod
    def bytes_to_bits(data: bytes) -> List[int]:
        """Convert byte array to list of bits (LSB first per byte)."""
        bits = []
        for b in data:
            for i in range(8):
                bits.append((b >> i) & 1)
        return bits

    @staticmethod
    def hdlc_bit_stuff(bits: List[int]) -> List[int]:
        """Insert a 0 bit after 5 consecutive 1 bits (HDLC bit stuffing)."""
        stuffed = []
        ones_count = 0
        for bit in bits:
            stuffed.append(bit)
            if bit == 1:
                ones_count += 1
                if ones_count == 5:
                    stuffed.append(0)  # Stuff zero
                    ones_count = 0
            else:
                ones_count = 0
        return stuffed

    def nrzi_encode(self, bits: List[int]) -> List[int]:
        """
        NRZI Encoding:
        - Bit 0: Level transition (1 -> 0 or 0 -> 1)
        - Bit 1: Hold level (no transition)
        """
        nrzi_levels = []
        level = self.current_nrzi_level
        for bit in bits:
            if bit == 0:
                level = 1 - level  # Toggle
            nrzi_levels.append(level)
        self.current_nrzi_level = level
        return nrzi_levels

    def modulate_nrzi_levels(self, nrzi_levels: List[int]) -> np.ndarray:
        """
        Generate continuous-phase audio PCM samples for given NRZI levels.
        """
        sps = self.samples_per_symbol
        freq_list = [self.mark_freq if lvl == 1 else self.space_freq for lvl in nrzi_levels]
        freq_seq = np.repeat(freq_list, sps)

        phase_increments = 2.0 * np.pi * freq_seq / self.sample_rate
        phases = self.phase + np.cumsum(phase_increments)
        self.phase = float(np.mod(phases[-1], 2.0 * np.pi))

        pcm_out = np.sin(phases).astype(np.float32)
        return pcm_out

    def modulate_frame(self, payload: bytes) -> Tuple[np.ndarray, Dict]:
        """
        Full modulation pipeline for an AX.25 / HDLC frame with PTT timing.
        """
        self._reset_state()

        # 1. Compute FCS (CRC-16)
        fcs = crc16_hdlc(payload)
        fcs_bytes = bytes([fcs & 0xFF, (fcs >> 8) & 0xFF])
        frame_bytes = payload + fcs_bytes

        # 2. Preamble Flags (0x7E) - Unstuffed
        flag_bits = self.bytes_to_bits(bytes([0x7E]) * self.tx_delay_flags)

        # 3. Bit-stuffed Frame Payload
        payload_bits = self.bytes_to_bits(frame_bytes)
        stuffed_payload_bits = self.hdlc_bit_stuff(payload_bits)

        # 4. Postamble Flags (0x7E) - Unstuffed
        postamble_bits = self.bytes_to_bits(bytes([0x7E]) * self.tx_tail_flags)

        # Combine all bit streams
        all_bits = flag_bits + stuffed_payload_bits + postamble_bits

        # 5. NRZI Encode
        nrzi_levels = self.nrzi_encode(all_bits)

        # 6. Synthesize Continuous-Phase Audio PCM
        pcm_samples = self.modulate_nrzi_levels(nrzi_levels)

        # Calculate PTT timing metadata
        sps = self.samples_per_symbol
        preamble_samples = len(flag_bits) * sps
        payload_samples = len(stuffed_payload_bits) * sps
        postamble_samples = len(postamble_bits) * sps

        ptt_info = {
            "ptt_pin": self.PTT_PIN,
            "ptt_active": True,
            "tx_pin": self.TX_PIN,
            "total_samples": len(pcm_samples),
            "preamble_duration_ms": (preamble_samples / self.sample_rate) * 1000.0,
            "tx_duration_ms": (len(pcm_samples) / self.sample_rate) * 1000.0,
            "preamble_samples": preamble_samples,
            "payload_samples": payload_samples,
            "postamble_samples": postamble_samples,
        }

        return pcm_samples, ptt_info


class AFSKDemodulator:
    """
    Core AFSK 1200bps Demodulator:
    - Ingests RX audio from Pin 31 (ADC)
    - Lowpass filter & 1-sample delay product frequency discriminator
    - Software DPLL clock recovery
    - NRZI decoding & HDLC frame extraction with CRC-16 check
    """

    RX_PIN = 31
    TX_PIN = 20
    PTT_PIN = 22

    def __init__(
        self,
        sample_rate: int = 9600,
        baud_rate: int = 1200,
        mark_freq: float = 1200.0,
        space_freq: float = 2200.0,
        dpll_alpha: float = 0.25,
    ):
        self.sample_rate = sample_rate
        self.baud_rate = baud_rate
        self.mark_freq = mark_freq
        self.space_freq = space_freq
        self.samples_per_symbol = int(round(sample_rate / baud_rate))
        self.dpll_alpha = dpll_alpha

        self.lpf = BiquadFilter.lowpass(1200.0, sample_rate, q=0.7071)
        self.reset()

    def reset(self):
        self.lpf = BiquadFilter.lowpass(1200.0, self.sample_rate, q=0.7071)
        self.prev_sample = 0.0
        self.dpll_counter = 0
        self.prev_sign = True
        self.prev_sampled_level = 1
        self.dc_avg = 0.20941

        self.bit_shift_reg = 0
        self.ones_count = 0
        self.in_frame = False
        self.rx_bit_buffer = []

    def filter_rx_audio(self, audio_samples: np.ndarray) -> np.ndarray:
        return audio_samples

    def discriminate_frequency(self, rx_audio: np.ndarray) -> np.ndarray:
        baseband = np.zeros(len(rx_audio), dtype=np.float32)
        for i, sample in enumerate(rx_audio):
            raw_prod = sample * self.prev_sample
            self.prev_sample = sample
            bb_raw = self.lpf.process_sample(raw_prod)
            self.dc_avg += 0.002 * (bb_raw - self.dc_avg)
            baseband[i] = bb_raw - self.dc_avg
        return baseband

    def dpll_clock_recovery(self, baseband: np.ndarray) -> List[Tuple[int, int]]:
        sampled_bits = []
        for i, val in enumerate(baseband):
            curr_sign = (val >= 0)
            if curr_sign != self.prev_sign:
                if 0 < self.dpll_counter <= 3:
                    self.dpll_counter -= 1
                elif 5 <= self.dpll_counter <= 7:
                    self.dpll_counter += 1
                self.prev_sign = curr_sign

            if self.dpll_counter == 4:
                bit_level = 1 if val >= 0 else 0
                sampled_bits.append((bit_level, i))

            self.dpll_counter += 1
            if self.dpll_counter >= 8:
                self.dpll_counter = 0

        return sampled_bits

    def decode_frames(self, sampled_bits: List[Tuple[int, int]]) -> List[bytes]:
        if len(sampled_bits) < 16:
            return []

        decoded_frames = []
        prev_level = self.prev_sampled_level

        for level, _ in sampled_bits:
            nrzi_bit = 0 if level != prev_level else 1
            prev_level = level

            self.bit_shift_reg = ((self.bit_shift_reg << 1) | nrzi_bit) & 0xFF

            if self.bit_shift_reg == 0x7E:
                if self.in_frame and len(self.rx_bit_buffer) >= 7 + 16:
                    bits_clean = self.rx_bit_buffer[:-7]
                    frame_bytes = self._bits_to_bytes(bits_clean)
                    if len(frame_bytes) >= 3:
                        payload = frame_bytes[:-2]
                        rx_fcs = frame_bytes[-2] | (frame_bytes[-1] << 8)
                        calc_fcs = crc16_hdlc(payload)
                        if rx_fcs == calc_fcs:
                            decoded_frames.append(payload)

                self.in_frame = True
                self.rx_bit_buffer = []
                self.ones_count = 0
                continue

            if self.in_frame:
                if nrzi_bit == 1:
                    self.ones_count += 1
                    if self.ones_count >= 7:
                        self.in_frame = False
                        self.rx_bit_buffer = []
                        self.ones_count = 0
                        continue
                    self.rx_bit_buffer.append(1)
                else:
                    if self.ones_count == 5:
                        self.ones_count = 0
                        continue
                    self.ones_count = 0
                    self.rx_bit_buffer.append(0)

        self.prev_sampled_level = prev_level
        return decoded_frames

    @staticmethod
    def _bits_to_bytes(bits: List[int]) -> bytes:
        byte_list = []
        for i in range(0, len(bits) - 7, 8):
            byte_val = 0
            for bit_idx in range(8):
                if bits[i + bit_idx]:
                    byte_val |= (1 << bit_idx)
            byte_list.append(byte_val)
        return bytes(byte_list)

    def process_rx_audio(self, audio_samples: np.ndarray) -> List[bytes]:
        self.reset()
        baseband = self.discriminate_frequency(audio_samples)
        sampled_bits = self.dpll_clock_recovery(baseband)
        decoded_frames = self.decode_frames(sampled_bits)
        return decoded_frames
