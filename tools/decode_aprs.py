import wave
import struct
import sys
import os
import numpy as np
import scipy.signal as signal

def crc16_hdlc(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
    return crc ^ 0xFFFF

def parse_ax25_frame(raw_bytes: bytes):
    if len(raw_bytes) < 16: # Min AX.25 frame size
        return None
    
    # Destination callsign & SSID
    dest_call = "".join([chr((b >> 1) & 0x7F) for b in raw_bytes[0:6]]).strip()
    dest_ssid = (raw_bytes[6] >> 1) & 0x0F
    
    # Source callsign & SSID
    src_call = "".join([chr((b >> 1) & 0x7F) for b in raw_bytes[7:13]]).strip()
    src_ssid = (raw_bytes[13] >> 1) & 0x0F
    
    path = []
    idx = 13
    has_digi = not (raw_bytes[13] & 0x01)
    while has_digi and idx + 7 <= len(raw_bytes):
        digi_call = "".join([chr((b >> 1) & 0x7F) for b in raw_bytes[idx+1:idx+7]]).strip()
        digi_ssid = (raw_bytes[idx+7] >> 1) & 0x0F
        h_bit = "*" if (raw_bytes[idx+7] & 0x80) else ""
        path.append(f"{digi_call}-{digi_ssid}{h_bit}")
        has_digi = not (raw_bytes[idx+7] & 0x01)
        idx += 7
    
    idx += 1
    if idx < len(raw_bytes):
        ctrl = raw_bytes[idx]
        idx += 1
    else:
        ctrl = None
        
    if idx < len(raw_bytes):
        pid = raw_bytes[idx]
        idx += 1
    else:
        pid = None
        
    info = raw_bytes[idx:] if idx <= len(raw_bytes) else b""
    
    path_str = "," + ",".join(path) if path else ""
    header = f"{src_call}-{src_ssid}>{dest_call}-{dest_ssid}{path_str}"
    
    try:
        info_str = info.decode('utf-8')
    except UnicodeDecodeError:
        info_str = info.decode('latin-1', errors='replace')
        
    return {
        "header": header,
        "src": f"{src_call}-{src_ssid}",
        "dest": f"{dest_call}-{dest_ssid}",
        "path": path,
        "info": info_str,
        "info_raw": info
    }

def demodulate_segment(bp_audio_segment: np.ndarray, target_fs: int = 9600):
    if len(bp_audio_segment) < 100:
        return []

    # Frequency Discriminator: 1-sample delay product
    prod = bp_audio_segment[1:] * bp_audio_segment[:-1]
    nyq = target_fs / 2.0
    b_lpf, a_lpf = signal.butter(2, 1200.0 / nyq, btype='lowpass')
    baseband = signal.lfilter(b_lpf, a_lpf, prod)
    baseband = baseband - np.mean(baseband)

    # DPLL Clock Recovery
    sps = 8.0
    dpll = 0.0
    prev_sign = (baseband[0] >= 0) if len(baseband) > 0 else True
    sampled_bits = []

    for val in baseband:
        curr_sign = (val >= 0)
        if curr_sign != prev_sign:
            err = (dpll + 0.5 * sps) % sps - 0.5 * sps
            dpll -= 0.5 * err
            prev_sign = curr_sign
        old_dpll = dpll
        dpll += 1.0
        if old_dpll < sps / 2.0 <= dpll:
            sampled_bits.append(1 if val >= 0 else 0)
        if dpll >= sps:
            dpll -= sps

    valid_frames = []
    
    for inv in [False, True]:
        nrzi_bits = []
        prev_l = sampled_bits[0] if sampled_bits else 0
        for b in sampled_bits:
            if not inv:
                bit = 0 if b != prev_l else 1
            else:
                bit = 1 if b != prev_l else 0
            nrzi_bits.append(bit)
            prev_l = b

        shift_reg = 0
        in_frame = False
        rx_bits = []
        ones_count = 0

        for bit in nrzi_bits:
            shift_reg = ((shift_reg << 1) | bit) & 0xFF
            if shift_reg == 0x7E:
                if in_frame and len(rx_bits) >= 7 + 16:
                    clean_bits = rx_bits[:-7]
                    b_list = []
                    for idx in range(0, len(clean_bits) - 7, 8):
                        v = 0
                        for bit_idx in range(8):
                            if clean_bits[idx + bit_idx]:
                                v |= (1 << bit_idx)
                        b_list.append(v)
                    fb = bytes(b_list)
                    if len(fb) >= 3:
                        payload = fb[:-2]
                        rx_fcs = fb[-2] | (fb[-1] << 8)
                        if rx_fcs == crc16_hdlc(payload):
                            if payload not in valid_frames:
                                valid_frames.append(payload)
                in_frame = True
                rx_bits = []
                ones_count = 0
                continue

            if in_frame:
                if bit == 1:
                    ones_count += 1
                    if ones_count >= 7:
                        in_frame = False
                        rx_bits = []
                        ones_count = 0
                        continue
                    rx_bits.append(1)
                else:
                    if ones_count == 5:
                        ones_count = 0
                        continue
                    ones_count = 0
                    rx_bits.append(0)

    return valid_frames

def decode_aprs_audio(audio_samples: np.ndarray, fs: int):
    if len(audio_samples) == 0:
        return []

    # 1. Normalize
    max_val = np.max(np.abs(audio_samples))
    if max_val > 0:
        samples = audio_samples / max_val
    else:
        samples = audio_samples

    # 2. Resample to 9600 Hz (8 samples / symbol at 1200 baud)
    target_fs = 9600
    if fs != target_fs:
        duration = len(samples) / fs
        num_target_samples = int(round(duration * target_fs))
        t_orig = np.linspace(0, duration, len(samples), endpoint=False)
        t_target = np.linspace(0, duration, num_target_samples, endpoint=False)
        samples_9600 = np.interp(t_target, t_orig, samples).astype(np.float32)
    else:
        samples_9600 = samples

    # 3. Bandpass Filter (900 Hz - 2500 Hz)
    nyq = target_fs / 2.0
    b_bp, a_bp = signal.butter(2, [900.0 / nyq, 2500.0 / nyq], btype='bandpass')
    bp_audio = signal.lfilter(b_bp, a_bp, samples_9600)

    # 4. Energy Gating / Carrier Detection
    window = int(0.01 * target_fs)
    env = np.array([np.std(bp_audio[max(0, i-window):min(len(bp_audio), i+window)]) for i in range(len(bp_audio))])
    
    threshold = max(0.02, 0.25 * np.max(env))
    active_mask = env > threshold

    diff_mask = np.diff(active_mask.astype(int))
    starts = np.where(diff_mask == 1)[0]
    ends = np.where(diff_mask == -1)[0]

    if active_mask[0]:
        starts = np.insert(starts, 0, 0)
    if len(active_mask) > 0 and active_mask[-1]:
        ends = np.append(ends, len(active_mask) - 1)

    all_decoded_frames = []

    # Demodulate each detected burst
    pad = int(0.05 * target_fs) # 50ms padding
    for s, e in zip(starts, ends):
        s_pad = max(0, s - pad)
        e_pad = min(len(bp_audio), e + pad)
        segment = bp_audio[s_pad:e_pad]
        frames = demodulate_segment(segment, target_fs)
        for f in frames:
            if f not in all_decoded_frames:
                all_decoded_frames.append(f)

    # If no frames decoded via energy gating, fallback to whole file
    if not all_decoded_frames:
        all_decoded_frames = demodulate_segment(bp_audio, target_fs)

    return all_decoded_frames

def process_file(file_path: str):
    if not os.path.exists(file_path):
        print(f"Error: File '{file_path}' does not exist.", file=sys.stderr)
        sys.exit(1)

    try:
        with wave.open(file_path, 'rb') as wf:
            n_channels = wf.getnchannels()
            sampwidth = wf.getsampwidth()
            fs = wf.getframerate()
            n_frames = wf.getnframes()
            raw_bytes = wf.readframes(n_frames)
    except Exception as e:
        print(f"Error reading WAV file '{file_path}': {e}", file=sys.stderr)
        sys.exit(1)

    if n_frames == 0:
        print(f"Error: WAV file '{file_path}' contains 0 audio frames.", file=sys.stderr)
        sys.exit(1)

    if sampwidth == 2:
        fmt = f"<{n_frames * n_channels}h"
        samples = np.array(struct.unpack(fmt, raw_bytes), dtype=np.float32) / 32768.0
    elif sampwidth == 1:
        fmt = f"<{n_frames * n_channels}B"
        samples = (np.array(struct.unpack(fmt, raw_bytes), dtype=np.float32) - 128.0) / 128.0
    elif sampwidth == 4:
        fmt = f"<{n_frames * n_channels}i"
        samples = np.array(struct.unpack(fmt, raw_bytes), dtype=np.float32) / 2147483648.0
    else:
        print(f"Error: Unsupported sample width ({sampwidth} bytes).", file=sys.stderr)
        sys.exit(1)

    if n_channels > 1:
        samples = samples.reshape(-1, n_channels)[:, 0]

    frames = decode_aprs_audio(samples, fs)

    if not frames:
        print(f"Error: No valid APRS/AX.25 frame decoded from '{file_path}'.", file=sys.stderr)
        sys.exit(1)

    print(f"Decoded {len(frames)} valid APRS frame(s) from '{file_path}':\n")
    for i, frame in enumerate(frames, start=1):
        print(f"=== Frame {i} ===")
        print(f"Hex: {frame.hex()}")
        parsed = parse_ax25_frame(frame)
        if parsed:
            print(f"Header: {parsed['header']}")
            print(f"Source: {parsed['src']}")
            print(f"Destination: {parsed['dest']}")
            print(f"Path: {','.join(parsed['path']) if parsed['path'] else 'None'}")
            print(f"Payload: {parsed['info']}")
        else:
            print(f"Raw Bytes: {frame}")
        print()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python decode_aprs.py <path_to_wav_file>")
        sys.exit(1)

    process_file(sys.argv[1])
