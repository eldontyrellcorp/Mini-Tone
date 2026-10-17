#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 <YOUR NAME>
"""FM DRUM's 62 built-in transients (firmware/src/eng_fmdrum.c) -> a C header of 8-bit mu-law.

  gen_transients.py OUT.h                 build the header
  gen_transients.py OUT.h --write-wavs D  ... and write the 62 WAVs to D (to audition them, or to start replacing some)

The bank is synthesized here (Felucca's own: nothing recorded, nothing third-party), 16-bit mono 44,100 Hz, 2 .. 45 ms
each, in ten families: clicks, noise ticks, rims and sticks, wood, metal, thumps, snare cracks, claps, digital bits,
zaps. A slot can be replaced with a recording of your own: put assets/transients/NN_NAME.wav (NN = 01 .. 62; any rate,
any bit depth, mono or stereo) in the repository; it is resampled to 44,100 Hz, trimmed to 100 ms, its DC removed, its
peak set to -1 dBFS and a short fade put on its end, as the synthesized ones are. NAME (6 characters at most) is the
slot's name in this header.

Stored as 8-bit mu-law (G.711) at the output rate, so the engine plays one decoded sample per output sample: no state,
no interpolation, no rate. (IMA ADPCM, which the sample sets use, was tried first: at 4 bit it spreads its error over the
whole spectrum, 4 .. 10 dB below a noise or click transient, which turns a high-passed tick into broadband noise.)
"""
import math
import random
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sampleio  # noqa: E402

SR = 44100
N = 62
MAX_MS = 100
PEAK = 0.89                                         # -1 dBFS
TWO_PI = 2 * math.pi


# --------------------------------------------------------------------------------------------- synthesis helpers --
def ms(t):
    return max(1, int(round(t * SR / 1000)))


def rnd(seed):
    return random.Random(seed)


def noise(r, n):
    return [r.uniform(-1, 1) for _ in range(n)]


def decay(n, t_ms):
    """exponential: -40 dB after t_ms"""
    tau = t_ms / 1000 / 4.6
    return [math.exp(-i / SR / tau) for i in range(n)]


def mul(a, b):
    return [x * y for x, y in zip(a, b)]


def add(*xs):
    n = max(len(x) for x in xs)
    return [sum(x[i] if i < len(x) else 0.0 for x in xs) for i in range(n)]


def gain(x, g):
    return [v * g for v in x]


def delay(x, t_ms):
    return [0.0] * ms(t_ms) + list(x)


def lp(x, fc):
    a = 1 - math.exp(-TWO_PI * fc / SR)
    y, out = 0.0, []
    for v in x:
        y += a * (v - y)
        out.append(y)
    return out


def hp(x, fc):
    l = lp(x, fc)
    return [v - w for v, w in zip(x, l)]


def bp(x, fc, q=2.0):
    """2-pole state variable band-pass"""
    f = 2 * math.sin(math.pi * min(fc, SR / 6) / SR)
    d = 1 / q
    low = band = 0.0
    out = []
    for v in x:
        low += f * band
        high = v - low - d * band
        band += f * high
        out.append(band)
    return out


def sine(n, f, ph=0.0):
    return [math.sin(TWO_PI * f * i / SR + ph) for i in range(n)]


def sweep(n, f0, f1, curve=1.0):
    """sine whose frequency goes f0 -> f1 (exponential in time for curve 1, shaped by curve)"""
    out, ph = [], 0.0
    for i in range(n):
        t = i / max(1, n - 1)
        f = f0 * (f1 / f0) ** (t ** curve)
        ph += TWO_PI * f / SR
        out.append(math.sin(ph))
    return out


def dsine(n, f, t_ms, ph=0.0):
    return mul(sine(n, f, ph), decay(n, t_ms))


def impulse(n, pos=0, amp=1.0):
    x = [0.0] * n
    x[pos] = amp
    return x


def square(n, f):
    return [1.0 if math.sin(TWO_PI * f * i / SR) >= 0 else -1.0 for i in range(n)]


def saw(n, f):
    return [2 * ((f * i / SR) % 1.0) - 1 for i in range(n)]


def crush(x, bits):
    q = 2 ** (bits - 1)
    return [round(v * q) / q for v in x]


def hold(x, k):
    return [x[(i // k) * k] for i in range(len(x))]


def red(x):
    y, out = 0.0, []
    for v in x:
        y += v * 0.12 - y * 0.008
        out.append(y)
    return out


def burst(r, n, t_ms, fc=None, kind="bp", q=1.5):
    z = noise(r, n)
    if fc and kind == "bp":
        z = bp(z, fc, q)
    elif fc and kind == "lp":
        z = lp(z, fc)
    elif fc and kind == "hp":
        z = hp(z, fc)
    return mul(z, decay(n, t_ms))


def partials(n, base, ratios, amps, t_ms):
    return add(*[gain(dsine(n, base * r, t_ms / (1 + 0.15 * k)), a) for k, (r, a) in enumerate(zip(ratios, amps))])


HAT = [1.0, 1.4471, 1.6170, 1.9265, 2.5028, 2.6637]          # the classic six inharmonic ratios


# ------------------------------------------------------------------------------------------------------ the bank --
def bank():
    B = []

    def slot(name, f):
        B.append((name, f))

    # A. clicks (8)
    slot("CLK1", lambda: lp(impulse(ms(3)), 8000))
    slot("CLK2", lambda: [-(i - 20) * math.exp(-((i - 20) / 6.5) ** 2) for i in range(60)])
    slot("CLK3", lambda: lp(impulse(ms(4)), 3000))
    slot("CLK4", lambda: hp(impulse(ms(3)), 4000))
    slot("DCLK", lambda: lp(add(impulse(ms(4)), impulse(ms(4), ms(1.2), -0.8)), 6000))
    slot("TICK", lambda: mul(sine(ms(2.5), 3500), [math.sin(math.pi * i / ms(2.5)) ** 2 for i in range(ms(2.5))]))
    slot("POP", lambda: mul(sine(ms(5), 100, 0), [math.sin(math.pi * i / ms(5)) for i in range(ms(5))]))
    slot("SNAP1", lambda: hp(mul([1.0] * ms(4), decay(ms(4), 1.6)), 1500))
    # B. noise ticks (8)
    slot("NZW1", lambda: burst(rnd(11), ms(4), 2.5))
    slot("NZW2", lambda: burst(rnd(12), ms(9), 6))
    slot("NZLP", lambda: burst(rnd(13), ms(8), 5, 2000, "lp"))
    slot("NZHP", lambda: burst(rnd(14), ms(6), 4, 6000, "hp"))
    slot("NZBP1", lambda: burst(rnd(15), ms(9), 6, 1500, "bp", 2.5))
    slot("NZBP2", lambda: burst(rnd(16), ms(7), 5, 4000, "bp", 2.5))
    slot("NZRD", lambda: mul(red(noise(rnd(17), ms(12))), decay(ms(12), 8)))
    slot("NZGA", lambda: add(*[delay(burst(rnd(18 + k), ms(1.2), 1), k * 2) for k in range(4)]))
    # C. rims and sticks (8)
    slot("RIM1", lambda: add(dsine(ms(10), 1700, 6), gain(burst(rnd(21), ms(10), 1.5), 0.5)))
    slot("RIM2", lambda: add(dsine(ms(9), 2600, 5), gain(dsine(ms(9), 1100, 6), 0.6), gain(burst(rnd(22), ms(9), 1), 0.3)))
    slot("RIM3", lambda: add(dsine(ms(7), 4000, 4), gain(burst(rnd(23), ms(7), 1, 6000, "hp"), 0.4)))
    slot("STK1", lambda: add(burst(rnd(24), ms(12), 2), gain(dsine(ms(12), 900, 10), 0.8)))
    slot("STK2", lambda: add(dsine(ms(12), 1400, 8), gain(dsine(ms(12), 2300, 6), 0.7)))
    slot("SIDE", lambda: add(dsine(ms(18), 480, 12), gain(dsine(ms(18), 1500, 9), 0.6), gain(burst(rnd(25), ms(18), 3), 0.3)))
    slot("CLAVE", lambda: mul(sweep(ms(30), 2600, 2400), decay(ms(30), 25)))
    slot("RIMHI", lambda: add(dsine(ms(6), 5500, 3), gain(burst(rnd(26), ms(6), 1, 8000, "hp"), 0.3)))
    # D. wood and blocks (6)
    slot("WOOD1", lambda: add(dsine(ms(22), 800, 15), gain(dsine(ms(22), 1520, 8), 0.5)))
    slot("WOOD2", lambda: add(dsine(ms(20), 1200, 14), gain(dsine(ms(20), 2300, 7), 0.5)))
    slot("WOOD3", lambda: add(dsine(ms(18), 1800, 12), gain(dsine(ms(18), 3400, 6), 0.4)))
    slot("BLOCK", lambda: add(dsine(ms(28), 1000, 20), gain(dsine(ms(28), 1500, 18), 0.7)))
    slot("TEMPLE", lambda: add(dsine(ms(35), 650, 25), gain(dsine(ms(35), 980, 22), 0.8)))
    slot("BONGO", lambda: mul(sweep(ms(30), 520, 380), decay(ms(30), 24)))
    # E. metal (8)
    slot("MET1", lambda: partials(ms(18), 1200, HAT, [1, .9, .8, .7, .6, .5], 12))
    slot("MET2", lambda: partials(ms(26), 1800, HAT, [1, .9, .8, .7, .6, .5], 20))
    slot("MET3", lambda: partials(ms(34), 800, HAT, [1, .9, .8, .7, .6, .5], 28))
    slot("CWBL", lambda: add(dsine(ms(32), 587, 24), dsine(ms(32), 845, 20)))
    slot("BELL", lambda: partials(ms(32), 900, [1.0, 2.76, 5.4], [1.0, 0.6, 0.35], 24))
    slot("TING", lambda: add(dsine(ms(22), 3000, 15), gain(dsine(ms(22), 6300, 8), 0.5)))
    slot("RIDE", lambda: add(burst(rnd(35), ms(32), 22, 5000, "hp"), gain(partials(ms(32), 1500, HAT, [1] * 6, 24), 0.8)))
    slot("SHIM", lambda: mul(partials(ms(28), 1400, HAT, [1] * 6, 20), [0.6 + 0.4 * x for x in noise(rnd(36), ms(28))]))
    # F. thumps (6)
    slot("KICK1", lambda: mul(sweep(ms(28), 220, 60, 0.5), decay(ms(28), 22)))
    slot("KICK2", lambda: mul(sweep(ms(30), 160, 45, 0.5), decay(ms(30), 22)))
    slot("BEAT", lambda: add(burst(rnd(41), ms(12), 6, 1500, "lp"), gain(mul(sine(ms(30), 90), decay(ms(30), 22)), 0.9)))
    slot("TOM1", lambda: mul(sweep(ms(28), 300, 120, 0.5), decay(ms(28), 20)))
    slot("TOM2", lambda: mul(sweep(ms(30), 200, 90, 0.5), decay(ms(30), 22)))
    slot("THUD", lambda: add(mul(sine(ms(30), 70), decay(ms(30), 22)), gain(burst(rnd(42), ms(30), 8, 600, "lp"), 0.8)))
    # G. snare cracks (6)
    slot("SNR1", lambda: add(burst(rnd(51), ms(28), 20, 2500, "bp", 1.0), gain(dsine(ms(28), 190, 14), 0.7)))
    slot("SNR2", lambda: add(burst(rnd(52), ms(28), 20, 1000, "hp"), gain(dsine(ms(28), 250, 14), 0.6)))
    slot("CRACK", lambda: add(burst(rnd(53), ms(10), 5, 8000, "hp"), gain(dsine(ms(10), 1200, 4), 0.7)))
    slot("WHIP", lambda: mul(bp(noise(rnd(54), ms(22)), 3000, 3.0), [math.sin(math.pi * (i / ms(22)) ** 0.5) * math.exp(-i / ms(22) * 3) for i in range(ms(22))]))
    slot("SNR3", lambda: add(burst(rnd(55), ms(32), 22, 4000, "lp"), gain(dsine(ms(32), 160, 16), 0.8)))
    slot("SHOT", lambda: add(burst(rnd(56), ms(16), 12), gain(dsine(ms(16), 330, 10), 0.6)))
    # H. claps (4)
    slot("CLAP1", lambda: add(*[delay(burst(rnd(61 + k), ms(24 - 4 * k), 3 if k < 2 else 18, 1200, "bp", 1.2), k * 5) for k in range(3)]))
    slot("CLAP2", lambda: add(*[delay(burst(rnd(64 + k), ms(26 - 4 * k), 3 if k < 3 else 16, 2000, "bp", 1.2), k * 4) for k in range(4)]))
    slot("CLAP3", lambda: add(*[delay(burst(rnd(68 + k), ms(30 - 8 * k), 4 if k < 1 else 20, 1600, "bp", 0.9), k * 8) for k in range(2)]))
    slot("SNAP2", lambda: add(burst(rnd(71), ms(18), 11, 2200, "bp", 2.0), gain(dsine(ms(18), 1800, 8), 0.5)))
    # I. digital (4)
    slot("BITS", lambda: crush(burst(rnd(81), ms(14), 9), 3))
    slot("SH", lambda: hold(burst(rnd(82), ms(16), 10), 5))
    slot("PULSE", lambda: mul(square(ms(14), 1500), decay(ms(14), 10)))
    slot("BUZZ", lambda: crush(mul(saw(ms(12), 600), decay(ms(12), 8)), 4))
    # J. zaps (4)
    slot("ZAPUP", lambda: mul(sweep(ms(14), 800, 6000), decay(ms(14), 10)))
    slot("ZAPDN", lambda: mul(sweep(ms(14), 7000, 600), decay(ms(14), 11)))
    slot("CHIRP", lambda: mul([math.sin(TWO_PI * sum(2000 * 2 ** (math.sin(math.pi * j / ms(16))) / SR for j in range(i))) for i in range(ms(16))], decay(ms(16), 12)))
    slot("BLIP", lambda: mul(square(ms(7), 2200), decay(ms(7), 4.5)))
    assert len(B) == N, len(B)
    return B


def mu_decode(b):
    """G.711 mu-law byte -> 16-bit sample (the firmware's mulaw_dec does the same)"""
    b = ~b & 0xFF
    t = (((b & 15) << 3) + 0x84) << ((b >> 4) & 7)
    return (0x84 - t) if b & 0x80 else (t - 0x84)


MU_TAB = sorted((mu_decode(b), b) for b in range(256))
MU_VAL = [v for v, _ in MU_TAB]


def mu_encode(s):
    """nearest of the 255 distinct levels"""
    import bisect
    i = bisect.bisect_left(MU_VAL, s)
    c = [k for k in (i - 1, i) if 0 <= k < 256]
    return MU_TAB[min(c, key=lambda k: abs(MU_VAL[k] - s))][1]


def finish(x):
    """DC removed, the tail below -38 dB cut, the last 1.5 ms faded, the peak set to -1 dBFS -> int16 list"""
    x = list(x)[:ms(MAX_MS)]
    m = sum(x) / len(x)
    x = [v - m for v in x]
    pk = max(1e-9, max(abs(v) for v in x))
    n = len(x)
    while n > ms(1.5) * 2 and abs(x[n - 1]) < 0.012 * pk:
        n -= 1
    x = x[:n]
    f = min(len(x) // 2, ms(1.5))
    for i in range(f):
        x[len(x) - 1 - i] *= 0.5 - 0.5 * math.cos(math.pi * i / f)
    pk = max(1e-9, max(abs(v) for v in x))
    return [max(-32768, min(32767, int(round(v / pk * PEAK * 32767)))) for v in x]


def load(assets):
    """-> [(name, int16 list)] of the N slots: the synthesized ones, a slot's WAV from assets/transients where there is one"""
    out = []
    for k, (name, f) in enumerate(bank()):
        mine = sorted(assets.glob(f"{k + 1:02d}_*.wav")) if assets.is_dir() else []
        if mine:
            sr, x = sampleio.read_any_wav(mine[0])
            x = sampleio.resample(x, sr, SR)
            name = mine[0].stem.split("_", 1)[1].upper()[:6]
            out.append((name, finish(x)))
        else:
            out.append((name, finish(f())))
    return out


def write_wav(path, s):
    data = b"".join(struct.pack("<h", v) for v in s)
    fmt = struct.pack("<HHIIHH", 1, 1, SR, SR * 2, 2, 16)
    body = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body)


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    out = Path(argv[0])
    root = Path(__file__).resolve().parent.parent
    sl = load(root / "assets" / "transients")
    blob, off, ln = bytearray(), [], []
    for _, s in sl:
        off.append(len(blob))
        ln.append(len(s))
        blob += bytes(mu_encode(v) for v in s)
    if len(blob) > 0xFFFF:
        raise SystemExit(f"gen_transients: {len(blob)} bytes of of mu-law: more than the 64 kB the 16-bit offsets address")
    L = ["/* generated by tools/gen_transients.py: FM DRUM's transients (8-bit mu-law, 44100 Hz) */", "#pragma once",
         f"#define TR_N {N}", f"#define TR_BYTES {len(blob)}",
         f"static const uint8_t TR_DATA[{len(blob)}] = {{"]
    for i in range(0, len(blob), 24):
        L.append("    " + ",".join(str(b) for b in blob[i:i + 24]) + ",")
    L.append("};")
    L.append("/* offset of each transient in TR_DATA (one byte a sample) and its length */")
    L.append(f"static const uint16_t TR_OFF[{N}] = {{" + ",".join(map(str, off)) + "};")
    L.append(f"static const uint16_t TR_LEN[{N}] = {{" + ",".join(map(str, ln)) + "};")
    L.append(f"static const char *const TR_NAME[{N}] = {{" + ",".join(f'"{n}"' for n, _ in sl) + "};")
    out.write_text("\n".join(L) + "\n")
    if "--write-wavs" in argv:
        d = Path(argv[argv.index("--write-wavs") + 1])
        d.mkdir(parents=True, exist_ok=True)
        for k, (n, s) in enumerate(sl):
            write_wav(d / f"{k + 1:02d}_{n}.wav", s)
    tot = sum(ln)
    print(f"transients: {N} slots, {tot} samples ({tot * 1000 / SR:.0f} ms, longest {max(ln) * 1000 / SR:.0f} ms), "
          f"{len(blob)} B mu-law")


if __name__ == "__main__":
    main(sys.argv[1:])
