#!/usr/bin/env python3
"""Measurements for Gridline's interface sounds (J-133). numpy only.

Used by tools/synth_ui_sounds.py to verify every file it writes, and usable on
its own:  python3 tools/uisound_measure.py assets/sounds/graphite/*.wav

What it reports per file (all levels for the file as written, before the app's
volume setting):
  sample peak (dBFS), true peak (dBTP, 4x band-limited oversampling),
  loudness per ITU-R BS.1770-4: max momentary (400 ms), max short-term (3 s)
  and integrated (gated) in LUFS; peak-to-loudness ratio (TP - M max);
  crest (sample peak over the RMS of the loudest 50 ms); DC offset; edges
  (first / last sample); onset rise (10 to 90 percent); tail (time until the
  10 ms envelope falls 60 dB under its peak); spectrum shares per band,
  centroid, the 2-5 kHz share and its strongest peak; stereo correlation,
  side-to-mid ratio and the loudness change when folded to mono.
"""
import math
import struct
import sys

import numpy as np

BANDS = [("sub", 20, 60), ("low", 60, 250), ("lowmid", 250, 800), ("mid", 800, 2000), ("presence", 2000, 5000),
         ("brilliance", 5000, 10000), ("air", 10000, 24000)]


# ------------------------------------------------------------------ files ---
def read_wav(path):
    """(sample rate, bits, channels, float64 array (n, 2)). Mono is duplicated."""
    with open(path, "rb") as f:
        data = f.read()
    form = data[:4]
    if form == b"FORM":  # AIFF (big-endian PCM), used only for reference measurements
        return _read_aiff(data)
    assert form == b"RIFF" and data[8:12] == b"WAVE", path
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE:
                fmt = (struct.unpack("<H", body[24:26])[0],) + fmt[1:]
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    tag, ch, sr, _, align, bits = fmt
    n = len(raw) // align
    raw = raw[:n * align]
    if tag == 3 and bits == 32:
        flat = np.frombuffer(raw, "<f4").astype(np.float64)
    elif tag == 3 and bits == 64:
        flat = np.frombuffer(raw, "<f8").astype(np.float64)
    elif bits == 16:
        flat = np.frombuffer(raw, "<i2") / 32768.0
    elif bits == 24:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        flat = v / 8388608.0
    elif bits == 32:
        flat = np.frombuffer(raw, "<i4") / 2147483648.0
    else:
        raise ValueError(f"unsupported wav format {fmt} in {path}")
    x = flat.reshape(-1, ch)
    if ch == 1:
        x = np.repeat(x, 2, axis=1)
    return sr, bits, ch, x[:, :2].astype(np.float64)


def _read_aiff(data):
    pos, ch, bits, sr, raw = 12, 0, 0, 0, b""
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack(">I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"COMM":
            ch, _, bits = struct.unpack(">hIh", body[:8])
            exp = struct.unpack(">H", body[8:10])[0] & 0x7FFF
            mant = struct.unpack(">Q", body[10:18])[0]
            sr = int(round(mant * 2.0 ** (exp - 16383 - 63)))
        elif cid == b"SSND":
            off = struct.unpack(">I", body[:4])[0]
            raw = body[8 + off:]
        pos += 8 + size + (size & 1)
    if bits == 16:
        flat = np.frombuffer(raw[:len(raw) // 2 * 2], ">i2") / 32768.0
    elif bits == 24:
        b = np.frombuffer(raw[:len(raw) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        v = (b[:, 0] << 16) | (b[:, 1] << 8) | b[:, 2]
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        flat = v / 8388608.0
    else:
        raise ValueError(f"unsupported aiff bits {bits}")
    x = flat[:len(flat) // ch * ch].reshape(-1, ch)
    if ch == 1:
        x = np.repeat(x, 2, axis=1)
    return sr, bits, ch, x[:, :2].astype(np.float64)


def write_wav24(path, x, sr):
    """Stereo 24-bit PCM. x: (n, 2) float, already inside +-1."""
    v = np.clip(np.round(np.asarray(x, np.float64) * 8388607.0), -8388608, 8388607).astype(np.int32).reshape(-1)
    b = np.empty((v.size, 3), np.uint8)
    u = v & 0xFFFFFF
    b[:, 0], b[:, 1], b[:, 2] = u & 0xFF, (u >> 8) & 0xFF, (u >> 16) & 0xFF
    payload = b.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, sr, sr * 6, 6, 24))
        f.write(b"data" + struct.pack("<I", len(payload)) + payload)


# ---------------------------------------------------------------- filters ---
def lfilter(b, a, x):
    """Direct form II transposed IIR, 1-D (plain loop; files are short)."""
    b = [c / a[0] for c in b]
    a = [c / a[0] for c in a]
    n = max(len(a), len(b))
    b += [0.0] * (n - len(b))
    a += [0.0] * (n - len(a))
    z = [0.0] * n
    out = np.empty(len(x))
    xs = np.asarray(x, np.float64).tolist()
    if n == 3:
        b0, b1, b2, a1, a2 = b[0], b[1], b[2], a[1], a[2]
        z1 = z2 = 0.0
        for i, v in enumerate(xs):
            y = b0 * v + z1
            z1 = b1 * v - a1 * y + z2
            z2 = b2 * v - a2 * y
            out[i] = y
        return out
    for i, v in enumerate(xs):
        y = b[0] * v + z[0]
        for k in range(1, n):
            z[k - 1] = b[k] * v - a[k] * y + (z[k] if k < n - 1 else 0.0)
        out[i] = y
    return out


def k_weighting(sr):
    """The two BS.1770 K-weighting biquads for any sample rate."""
    G, f0, Q = 3.999843853973347, 1681.974450955533, 0.7071752369554196
    K = math.tan(math.pi * f0 / sr)
    Vh = 10 ** (G / 20)
    Vb = Vh ** 0.4996667741545416
    a0 = 1 + K / Q + K * K
    s1 = ([(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0],
          [1.0, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0])
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = math.tan(math.pi * f0 / sr)
    a0 = 1 + K / Q + K * K
    s2 = ([1.0, -2.0, 1.0], [1.0, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0])
    return s1, s2


def kw(x, sr):
    s1, s2 = k_weighting(sr)
    return np.stack([lfilter(*s2, lfilter(*s1, x[:, c])) for c in range(x.shape[1])], axis=1)


# --------------------------------------------------------------- loudness ---
def _block_power(y, win, hop):
    """Mean square summed over channels for every window (y already K-weighted)."""
    p = (y * y).sum(axis=1)
    acc = np.concatenate([[0.0], np.cumsum(p)])
    starts = np.arange(0, len(p) - win + 1, hop)
    return (acc[starts + win] - acc[starts]) / win


def _lufs(ms):
    return -0.691 + 10 * np.log10(np.maximum(ms, 1e-20))


def loudness(x, sr):
    """(max momentary, max short-term, integrated) LUFS. The signal is padded
    with silence so a window can sit anywhere over a short sound."""
    pad = int(0.4 * sr)
    # the K filter's tail after the sound is captured by the trailing pad
    y = kw(np.concatenate([x, np.zeros((pad, 2))]), sr)
    p = (y * y).sum(axis=1)
    acc = np.concatenate([[0.0], np.cumsum(p)])
    N = len(p)

    def windows(win, hop):  # windows may hang over either end (silence there)
        s0 = np.arange(-win + hop, N, hop)
        return (acc[np.clip(s0 + win, 0, N)] - acc[np.clip(s0, 0, N)]) / win

    m = windows(int(0.4 * sr), int(0.01 * sr))
    s = windows(int(3.0 * sr), int(0.05 * sr))
    blocks = _block_power(y, int(0.4 * sr), int(0.1 * sr)) if N >= int(0.4 * sr) else windows(int(0.4 * sr), int(0.1 * sr))
    lk = _lufs(blocks)
    g = blocks[lk > -70]
    integ = -120.0
    if g.size:
        rel = _lufs(g.mean()) - 10
        g2 = g[_lufs(g) > rel]
        integ = float(_lufs(g2.mean())) if g2.size else -120.0
    return float(_lufs(m.max())), float(_lufs(s.max())), integ


def loudness_window(x, sr, seconds):
    """Max K-weighted loudness over any window of `seconds` (LUFS scale). With
    0.1 s it follows how loud a short hit feels better than the 400 ms
    momentary window, which averages a 150 ms sound with silence."""
    y = kw(np.concatenate([x, np.zeros((int(0.4 * sr), 2))]), sr)
    p = (y * y).sum(axis=1)
    acc = np.concatenate([[0.0], np.cumsum(p)])
    w = int(seconds * sr)
    if len(p) <= w:
        return float(_lufs(acc[-1] / w))
    return float(_lufs(((acc[w:] - acc[:-w]) / w).max()))


def true_peak(x, over=4):
    """Band-limited (FFT) 4x oversampled peak, dBTP."""
    pad = 256
    best = 0.0
    for c in range(x.shape[1]):
        v = np.concatenate([np.zeros(pad), x[:, c], np.zeros(pad)])
        n = len(v)
        n2 = 1 << int(math.ceil(math.log2(n)))
        spec = np.fft.rfft(v, n2)
        up = np.fft.irfft(spec, n2 * over) * over
        best = max(best, float(np.abs(up).max()))
    return 20 * math.log10(best) if best > 0 else -120.0


def db(v):
    return 20 * math.log10(v) if v > 0 else -120.0


# ---------------------------------------------------------------- analyse ---
def envelope_db(x, sr, ms=10.0):
    w = max(1, int(sr * ms / 1000))
    p = (x * x).mean(axis=1)
    acc = np.concatenate([[0.0], np.cumsum(p)])
    idx = np.arange(0, len(p) - w + 1, max(1, w // 2))
    return idx / sr, 10 * np.log10(np.maximum((acc[idx + w] - acc[idx]) / w, 1e-20))


def spectrum(x, sr):
    """Power spectrum of the mid signal (Hann, whole file), bins (Hz), power."""
    m = x.mean(axis=1)
    n = 1 << int(math.ceil(math.log2(max(len(m), 4096))))
    w = np.hanning(len(m))
    p = np.abs(np.fft.rfft(m * w, n)) ** 2
    return np.fft.rfftfreq(n, 1.0 / sr), p


def analyse(x, sr, bits=24, ch=2):
    n = len(x)
    out = {"sr": sr, "bits": bits, "channels": ch, "length_s": round(n / sr, 3)}
    pk = float(np.abs(x).max())
    out["peak_dbfs"] = round(db(pk), 2)
    out["true_peak_dbtp"] = round(true_peak(x), 2)
    mm, st, integ = loudness(x, sr)
    out["lufs_m_max"], out["lufs_s_max"], out["lufs_i"] = round(mm, 2), round(st, 2), round(integ, 2)
    out["lufs_100ms_max"] = round(loudness_window(x, sr, 0.1), 2)
    out["plr_db"] = round(out["true_peak_dbtp"] - mm, 1)
    w = max(1, int(0.05 * sr))
    p = (x * x).mean(axis=1)
    acc = np.concatenate([[0.0], np.cumsum(p)])
    rms50 = math.sqrt(max(((acc[w:] - acc[:-w]) / w).max(), 1e-30)) if n > w else math.sqrt(p.mean())
    out["crest_db"] = round(db(pk) - db(rms50), 1)
    out["dc_dbfs"] = round(db(float(np.abs(x.mean(axis=0)).max())), 1)
    out["edge_start_dbfs"] = round(db(float(np.abs(x[0]).max())), 1)
    out["edge_end_dbfs"] = round(db(float(np.abs(x[-1]).max())), 1)
    # onset: 0.5 ms RMS envelope over the first 60 ms
    t, e = envelope_db(x[:int(0.06 * sr)], sr, 0.5)
    lin = 10 ** (e / 20)
    top = lin.max()
    i10 = int(np.argmax(lin >= 0.1 * top))
    i90 = int(np.argmax(lin >= 0.9 * top))
    out["rise_ms"] = round((t[i90] - t[i10]) * 1000, 2)
    # tail: 10 ms envelope; time from the start until it last sits within 60 dB of its peak
    t, e = envelope_db(x, sr, 10.0)
    top = e.max()
    above = np.where(e > top - 60)[0]
    out["tail_60_s"] = round(float(t[above[-1]]) + 0.01, 3) if above.size else 0.0
    above40 = np.where(e > top - 40)[0]
    out["tail_40_s"] = round(float(t[above40[-1]]) + 0.01, 3) if above40.size else 0.0
    # level 30 ms before the end, relative to the peak envelope: a high value = tail cut by the file end
    k = max(0, len(e) - 1 - int(0.03 / 0.005))
    out["end_level_db"] = round(float(e[k] - top), 1)
    # clicks: energy above 8 kHz in 2 ms frames; the strongest frame relative to
    # the strongest broadband 2 ms frame. Smooth material stays far below 0 dB.
    f, pw = spectrum(x, sr)
    tot = pw[(f >= 20)].sum() or 1e-30
    bands = {}
    for name, lo, hi in BANDS:
        bands[name] = round(100 * pw[(f >= lo) & (f < hi)].sum() / tot, 2)
    out["bands_pct"] = bands
    out["centroid_hz"] = int(round(float((f * pw).sum() / (pw.sum() or 1e-30))))
    sel = (f >= 2000) & (f < 5000)
    out["presence_peak_rel_db"] = round(10 * math.log10(pw[sel].max() / pw.max()), 1) if pw.max() > 0 else -120.0
    out["sub_db"] = round(10 * math.log10(max(pw[(f >= 1) & (f < 60)].sum(), 1e-30) / tot), 1)
    # 8 kHz+ click detector (FFT high-pass, then 2 ms frames)
    spec = np.fft.rfft(x, axis=0)
    fr = np.fft.rfftfreq(n, 1.0 / sr)
    hf = np.fft.irfft(spec * (fr >= 8000)[:, None], n, axis=0)
    fw = max(1, int(0.002 * sr))
    nf = n // fw
    if nf:
        eb = (x[:nf * fw] ** 2).sum(axis=1).reshape(nf, fw).sum(axis=1)
        eh = (hf[:nf * fw] ** 2).sum(axis=1).reshape(nf, fw).sum(axis=1)
        out["hf8k_frame_db"] = round(10 * math.log10(max(eh.max(), 1e-30) / max(eb.max(), 1e-30)), 1)
    # stereo
    L, R = x[:, 0], x[:, 1]
    sll, srr = float((L * L).sum()), float((R * R).sum())
    out["corr"] = round(float((L * R).sum()) / math.sqrt(sll * srr), 3) if sll > 0 and srr > 0 else 1.0
    mid, side = (L + R) / 2, (L - R) / 2
    sm, ss = float((mid * mid).sum()), float((side * side).sum())
    out["side_db"] = round(10 * math.log10(ss / sm), 1) if ss > 0 and sm > 0 else -120.0
    mono = np.stack([mid, mid], axis=1)
    out["mono_change_db"] = round(loudness(mono, sr)[0] - mm, 2)
    return out


def measure_file(path):
    sr, bits, ch, x = read_wav(path)
    return analyse(x, sr, bits, ch)


if __name__ == "__main__":
    import json
    for p in sys.argv[1:]:
        print(p, json.dumps(measure_file(p)))
