#!/usr/bin/env python3
"""
Generate halfband FIR coefficients for 7-stage ×2 cascade interpolator.
44100 → 88200 → 176400 → 352800 → 705600 → 1411200 → 2822400 → 5644800 Hz
"""
import numpy as np
from scipy.signal import kaiserord, firwin, freqz
import sys

PASSBAND_HZ = 20000.0  # audio band limit

stages = [
    (44100,   88200,   120.0),   # stage 1: tightest transition band
    (88200,   176400,  120.0),
    (176400,  352800,  120.0),
    (352800,  705600,  120.0),
    (705600,  1411200, 120.0),
    (1411200, 2822400, 120.0),
    (2822400, 5644800, 120.0),
]

def design_halfband(fs_in, fs_out, atten_db):
    """Design halfband FIR for ×2 interpolation at fs_out.
    Passband: 0 .. PASSBAND_HZ
    Stopband: fs_in - PASSBAND_HZ .. fs_in  (mirror image to suppress)
    """
    assert fs_out == 2 * fs_in
    nyq = fs_out / 2.0  # = fs_in

    f_pass = PASSBAND_HZ
    f_stop = fs_in - PASSBAND_HZ

    # If transition band is very wide relative to fs (later stages) just use
    # a minimal filter.  For halfband property we force midpoint = fs_in/2.
    if f_stop <= f_pass:
        f_stop = 0.499 * fs_in
        f_pass = 0.001 * fs_in

    delta_f_norm = (f_stop - f_pass) / fs_out   # normalized to fs_out

    N, beta = kaiserord(atten_db, delta_f_norm)
    if N % 2 == 0:
        N += 1
    # Halfband requires N ≡ 1 (mod 4)
    while N % 4 != 1:
        N += 2

    # Design at fs_out, cutoff at halfband midpoint = fs_in/2
    cutoff_norm = (f_pass + f_stop) / 2.0 / (fs_out / 2.0)

    h = firwin(N, cutoff_norm, window=('kaiser', beta))
    # Normalize for unity DC gain (sum should already = 1 for firwin)
    h /= h.sum()
    return h, N, beta

print("// ============================================================")
print("// AUTO-GENERATED — do not edit by hand")
print("// gen_halfband_coeffs.py")
print("//")
print("// 7-stage halfband FIR cascade, ×2 each stage = ×128 total")
print("// Input: 44100 Hz   Output: 5644800 Hz (DSD128 step rate)")
print("// Audio passband: 0–20 kHz, stopband ≥ 120 dB")
print("// ============================================================")
print()

for idx, (fs_in, fs_out, atten) in enumerate(stages):
    h, N, beta = design_halfband(fs_in, fs_out, atten)
    C = N // 2  # center index

    # Halfband: h[C±2k] are non-zero, h[C±(2k+1)] ≈ 0
    # Extract the non-zero ODD-polyphase coefficients (distance 1,3,5,... from center)
    # These are the coefficients h[C-1], h[C-3], ..., h[0]  (half; symmetric)
    # Center coefficient h[C] is ideally exactly 0.5; we enforce it.
    odd_coeffs = []
    for k in range(1, C+1, 2):   # distance 1, 3, 5, ...
        odd_coeffs.append(h[C - k])   # one side only; other side = same by symmetry

    n_odd = len(odd_coeffs)

    # Verify stopband
    w, H = freqz(h, worN=8192, fs=fs_out)
    f_stop_hz = fs_in - PASSBAND_HZ
    mask = w >= f_stop_hz
    if mask.any():
        worst_stopband_db = 20 * np.log10(np.abs(H[mask]).max() + 1e-300)
    else:
        worst_stopband_db = -999.0

    print(f"// Stage {idx+1}: {fs_in}→{fs_out} Hz | N={N} taps | β={beta:.4f} |"
          f" stopband {worst_stopband_db:.1f} dB | odd_coeffs={n_odd}")
    print(f"static constexpr int   STAGE{idx+1}_TAPS     = {N};")
    print(f"static constexpr int   STAGE{idx+1}_ODD_N    = {n_odd};")
    print(f"// Odd-polyphase coefficients h[C-1], h[C-3], ..., h[0]")
    print(f"// (symmetric: h[C+k] = h[C-k])")
    print(f"static constexpr float STAGE{idx+1}_ODD[] = {{")
    per_line = 4
    for i in range(0, n_odd, per_line):
        chunk = odd_coeffs[i:i+per_line]
        line = "    " + ", ".join(f"{v:.12e}f" for v in chunk)
        if i + per_line < n_odd:
            line += ","
        print(line)
    print("};")
    print()
