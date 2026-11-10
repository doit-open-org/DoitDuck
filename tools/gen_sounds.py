#!/usr/bin/env python3
"""Synthesize the duck's sounds into sounds/*.wav (16 kHz, mono, 16-bit).

The voice is Microduck's: the `sounds` crate of pollen-robotics/microduck (Apache-2.0,
https://github.com/pollen-robotics/microduck/tree/main/sounds), ported from Rust to plain
Python. A seed derives a personality (pitch, timbre, nasality, vibrato, quackiness, tempo);
each recipe paints with it, so another seed is another duck. Changes from the original:
16 kHz instead of 48 kHz (the two sample-count constants rescaled as the original does),
harmonics fading out below the Nyquist frequency, the `wheee` ride left out (longer than
the 3 s a sound may last here), and our sound names mapped onto its recipes (SOUNDS).
Nothing is recorded. Only the Python standard library is needed:

    python3 tools/gen_sounds.py [out_dir] [--seed N]     # default: sounds/, seed 146
"""
import math
import os
import struct
import sys
import wave
import zlib

RATE = 16000
TUNED = 22050.0  # the rate the original recipes were tuned at
M64 = (1 << 64) - 1
TAU = 2 * math.pi


class Rng:
    """xoshiro256++ seeded by splitmix64, as the original's rng.rs."""

    def __init__(self, seed):
        x = seed & M64
        s = []
        for _ in range(4):
            x = (x + 0x9E3779B97F4A7C15) & M64
            z = x
            z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
            z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
            s.append(z ^ (z >> 31))
        self.s = s
        self.spare = None

    def next_u64(self):
        s = self.s
        rotl = lambda v, k: ((v << k) | (v >> (64 - k))) & M64
        result = (rotl((s[0] + s[3]) & M64, 23) + s[0]) & M64
        t = (s[1] << 17) & M64
        s[2] ^= s[0]
        s[3] ^= s[1]
        s[1] ^= s[2]
        s[0] ^= s[3]
        s[2] ^= t
        s[3] = rotl(s[3], 45)
        return result

    def random(self):
        return (self.next_u64() >> 11) * (1.0 / (1 << 53))

    def uniform(self, lo, hi):
        return lo + (hi - lo) * self.random()

    def integers(self, lo, hi):
        return lo + math.floor(self.random() * (hi - lo))

    def choice(self, xs):
        return xs[self.integers(0, len(xs))]

    def normal(self):
        if self.spare is not None:
            z, self.spare = self.spare, None
            return z
        r = 1.0 - self.random()
        theta = TAU * self.random()
        mag = math.sqrt(-2.0 * math.log(r))
        self.spare = mag * math.sin(theta)
        return mag * math.cos(theta)


class Personality:
    def __init__(self, seed):
        r = Rng(seed)
        self.seed = seed
        self.register = r.choice([-1.0, 0.0, 0.0, 1.0]) + r.uniform(-0.4, 0.4)
        base = r.uniform(160.0, 380.0)
        self.pitch = min(620.0, max(110.0, base * 2 ** (self.register * 0.45)))
        self.spread = r.uniform(0.4, 1.2)
        self.glide = r.uniform(-1.0, 1.0)
        self.brightness = r.uniform(0.05, 0.55)
        self.tilt = r.uniform(1.4, 2.8)
        self.nasal = r.uniform(0.1, 1.0)
        self.skew = r.uniform(-1.0, 1.0)
        self.formant_n = r.integers(1, 6)
        self.formant_gain = r.uniform(0.0, 1.4)
        self.vib_rate = r.uniform(3.5, 9.5)
        self.vib_depth = r.uniform(0.0, 0.7)
        self.jitter = r.uniform(0.03, 0.35)
        self.breath = r.uniform(0.0, 0.30)
        self.quackiness = r.uniform(0.2, 1.0)
        self.am_rate = r.uniform(18.0, 55.0)
        self.am_depth = r.uniform(0.15, 0.70)
        self.warble_hz = r.uniform(7.0, 18.0)
        self.warble_depth = r.uniform(0.0, 1.4)
        self.sharpness = r.uniform(0.0, 1.0)
        self.speed = r.uniform(0.82, 1.22)

    def soft(self, **kw):
        """A copy with some traits changed, as the recipes soften the voice per sound."""
        p = object.__new__(Personality)
        p.__dict__.update(self.__dict__, **kw)
        return p

    def variant_rng(self, tag, variant):
        h = ((self.seed * 1000003) & M64) ^ zlib.crc32(tag.encode()) ^ ((variant * 2654435761) & M64)
        return Rng(h & 0xFFFFFFFF)

    def harmonics(self):
        w = []
        for n in range(1, 8):
            base = 1.0 / n ** self.tilt
            lift = self.brightness * (n / 7) ** 1.5
            nasal = self.nasal * (0.6 if n in (2, 3) else 0.0)
            if self.skew >= 0:
                skew = self.skew * (0.4 if n % 2 == 0 else -0.2)
            else:
                skew = -self.skew * (-0.3 if n % 2 == 0 else 0.4)
            formant = self.formant_gain if n == self.formant_n else 0.0
            w.append(max(0.0, base + lift + nasal + skew + formant * base * 1.5))
        w[0] = max(w[0], 0.7)
        return w


# ---- the DSP primitives (synth.rs) ----

def t_axis(seconds):
    return [i / RATE for i in range(max(1, round(seconds * RATE)))]


def lerp(t, points):
    out = []
    for x in t:
        if x <= points[0][0]:
            out.append(points[0][1])
            continue
        for (x0, y0), (x1, y1) in zip(points, points[1:]):
            if x <= x1:
                out.append(y1 if x1 <= x0 else y0 + (y1 - y0) * (x - x0) / (x1 - x0))
                break
        else:
            out.append(points[-1][1])
    return out


def expdecay(t, attack, decay):
    attack, decay = max(attack, 1e-4), max(decay, 1e-4)
    return [min(1.0, max(0.0, x / attack)) * math.exp(-max(0.0, x - attack) / decay) for x in t]


def bell(t, attack, release):
    start = max(0.0, t[-1] - release)
    out = []
    for x in t:
        v = 1.0
        if x < attack:
            v = x / max(attack, 1e-4)
        if x > start:
            v = max(0.0, 1.0 - (x - start) / max(release, 1e-4))
        out.append(min(1.0, max(0.0, v)))
    return out


def vibrato(t, rate, depth, phase):
    if rate <= 0 or depth <= 0:
        return [1.0] * len(t)
    return [2 ** (depth * math.sin(TAU * rate * x + phase) / 12) for x in t]


def jitter(t, depth, rng):
    if depth <= 0:
        return [1.0] * len(t)
    raw = [rng.normal() for _ in t]
    k = max(1, round(64 * RATE / TUNED))  # the original's 64 samples at 22.05 kHz
    off, n, acc, out = (k - 1) // 2, len(raw), 0.0, []
    # numpy convolve(raw, ones(k)/k, 'same'): a centred moving average with zero padding.
    prefix = [0.0]
    for v in raw:
        acc += v
        prefix.append(acc)
    for i in range(n):
        hi, lo = min(n, i + off + 1), max(0, i + off + 1 - k)
        out.append(2 ** (depth * (prefix[hi] - prefix[lo]) / k / 12))
    return out


def pink_noise(n, rng):
    a = 0.985 ** (TUNED / RATE)
    leak, out = 0.0, []
    for _ in range(n):
        leak = a * leak + rng.normal()
        out.append(leak)
    peak = max(abs(v) for v in out) + 1e-9
    return [v / peak for v in out]


def click(n, rng, length):
    length = min(length, n)
    return [rng.uniform(-1, 1) * (1 - i / length) ** 2 if i < length else 0.0 for i in range(n)]


def normalise(x, peak_dbfs):
    g = 10 ** (peak_dbfs / 20) / (max(abs(v) for v in x) + 1e-9)
    return [v * g for v in x]


def voice(p, t, freq, rng, am_scale, breath_scale):
    """Harmonics of the gliding pitch with vibrato and jitter, the quacky buzz, and breath."""
    vib = vibrato(t, p.vib_rate, p.vib_depth, rng.uniform(0, TAU))
    jit = jitter(t, p.jitter, rng)
    f = [a * b * c for a, b, c in zip(freq, vib, jit)]
    weights = p.harmonics()
    body, acc = [], 0.0
    for fi in f:
        acc += fi
        ph = TAU * acc / RATE
        v = 0.0
        for n, w in enumerate(weights, 1):
            # Fade out what would fold back past the Nyquist frequency at 16 kHz.
            g = min(1.0, max(0.0, (0.47 * RATE - n * fi) / (0.05 * RATE)))
            if w and g:
                v += w * g * math.sin(n * ph)
        body.append(v)
    am_d = p.am_depth * am_scale * p.quackiness
    if am_d > 0.01:
        body = [b * (1 - am_d * (0.5 + 0.5 * math.sin(TAU * p.am_rate * x))) for b, x in zip(body, t)]
    breath = p.breath * breath_scale
    if breath > 0:
        body = [b + breath * nz for b, nz in zip(body, pink_noise(len(t), rng))]
    return body


def attack(p, dur, snappy):
    soft, sharp = 0.04 * dur, 0.003 * dur
    return max(0.001, soft + (sharp - soft) * p.sharpness * snappy)


def mul(a, b):
    return [x * y for x, y in zip(a, b)]


# ---- the recipes (voices.rs) ----

def alarm(p, variant):
    r = p.variant_rng('alarm', variant)
    dur = (0.20 + 0.12 * r.random()) / p.speed
    t = t_axis(dur)
    f0 = p.pitch * (1.25 + 0.35 * p.spread) * (0.94 + 0.12 * r.random())
    peak = 1.15 + 0.25 * p.spread + 0.10 * r.random()
    fall = 0.75 + 0.20 * (1 - p.spread)
    freq = lerp(t, [(0, f0), (0.05 * dur, f0 * peak), (dur, f0 * fall)])
    env = expdecay(t, attack(p, dur, 1.0), dur * (0.40 + 0.20 * r.random()))
    sig = mul(voice(p, t, freq, r, 0.5, 1.0), env)
    crackle = 0.04 + 0.10 * p.brightness
    sig = [v + crackle * r.normal() * e for v, e in zip(sig, env)]
    return normalise(sig, -3)


def greet_syllable(p, r, dur_scale, f0_scale):
    dur = (0.32 + 0.25 * r.random()) * dur_scale / p.speed
    t = t_axis(dur)
    f0 = p.pitch * (0.9 + 0.15 * r.random()) * f0_scale
    bend = 0.10 + 0.15 * p.spread
    start, mid = f0 * (1 - p.glide * bend * 0.5), f0 * (1 + p.glide * bend)
    end = f0 * (1 - p.glide * bend * 0.3) * (0.92 + 0.08 * r.random())
    freq = lerp(t, [(0, start), (0.18 * dur, mid), (dur, end)])
    return mul(voice(p, t, freq, r, 1.0, 1.0), expdecay(t, attack(p, dur, 0.5), dur * 0.7))


def greet(p, variant):
    """A quack, double ("wak-wak") on some variants; (samples, is_double)."""
    r = p.variant_rng('greet', variant)
    sig = greet_syllable(p, r, 1.0, 1.0)
    double = r.random() < 0.4
    if double:
        sig += [0.0] * int((0.05 + 0.06 * r.random()) / p.speed * RATE)
        sig += greet_syllable(p, r, 0.8, 0.95 + 0.06 * r.random())
    return normalise(sig, -3), double


def inquire(p, variant):
    r = p.variant_rng('inquire', variant)
    dur = (0.42 + 0.25 * r.random()) / p.speed
    t = t_axis(dur)
    f0 = p.pitch * (0.88 + 0.10 * r.random())
    rise = 1.15 + 0.50 * p.spread + 0.20 * max(0.0, p.glide) + 0.10 * r.random()
    freq = lerp(t, [(0, f0 * 0.92), (0.30 * dur, f0 * 0.95), (dur, f0 * rise)])
    env = bell(t, dur * (0.06 + 0.10 * (1 - p.sharpness)), dur * 0.32)
    return normalise(mul(voice(p, t, freq, r, 0.6, 1.0), env), -3)


def peck(p, variant):
    r = p.variant_rng('peck', variant)
    dur = (0.16 + 0.12 * r.random()) / p.speed
    t = t_axis(dur)
    f0 = p.pitch * (0.45 + 0.20 * r.random())
    freq = lerp(t, [(0, f0 * 1.5), (0.04 * dur, f0), (dur, f0 * 0.80)])
    body = mul(voice(p, t, freq, r, 0.3, 0.5), expdecay(t, attack(p, dur, 1.0), dur * 0.35))
    gain = 0.4 + 0.4 * p.sharpness
    body = [b + gain * c for b, c in zip(body, click(len(t), r, int((0.003 + 0.006 * p.sharpness) * RATE)))]
    return normalise(body, -3)


def chirp_syllable(p, r, f0, dur, contour):
    t = t_axis(dur)
    warble = vibrato(t, p.warble_hz, p.warble_depth, r.uniform(0, TAU))
    freq = [f0 * c * w for c, w in zip(lerp(t, contour), warble)]
    env = expdecay(t, attack(p, dur, 0.7), dur * 0.55)
    soft = p.soft(quackiness=p.quackiness * 0.2, brightness=p.brightness * 0.5, formant_gain=p.formant_gain * 0.5)
    return mul(voice(soft, t, freq, r, 1.0, 0.4), env)


def chirp(p, variant):
    """Rise, fall, trill or a double "wek-wek", by variant % 4."""
    r = p.variant_rng('chirp', variant)
    shape = variant % 4
    f0 = p.pitch * (0.95 + 0.75 * r.random())
    if shape == 0:
        dur = (0.10 + 0.10 * r.random()) / p.speed
        sig = chirp_syllable(p, r, f0, dur, [(0, 0.88), (0.5 * dur, 1.12 + 0.10 * r.random()), (dur, 1.05)])
    elif shape == 1:
        dur = (0.10 + 0.10 * r.random()) / p.speed
        sig = chirp_syllable(p, r, f0, dur, [(0, 1.12 + 0.10 * r.random()), (0.3 * dur, 1.0), (dur, 0.78)])
    elif shape == 2:
        dur = (0.22 + 0.14 * r.random()) / p.speed
        trill = p.soft(warble_depth=max(p.warble_depth, 0.7) * 1.6)
        sig = chirp_syllable(trill, r, f0, dur, [(0, 1.0), (0.5 * dur, 1.06), (dur, 0.94)])
    else:
        dur = (0.08 + 0.05 * r.random()) / p.speed
        sig = chirp_syllable(p, r, f0, dur, [(0, 0.95), (0.4 * dur, 1.10), (dur, 0.88)])
        sig += [0.0] * int((0.03 + 0.03 * r.random()) / p.speed * RATE)
        dur_b, f0_b = dur * 0.9, f0 * (0.92 + 0.10 * r.random())
        sig += chirp_syllable(p, r, f0_b, dur_b, [(0, 0.95), (0.4 * dur_b, 1.10), (dur_b, 0.88)])
    return normalise(sig, -6)


def coo(p, variant):
    r = p.variant_rng('coo', variant)
    dur = (0.85 + 0.55 * r.random()) / p.speed
    t = t_axis(dur)
    f0 = p.pitch * (0.42 + 0.15 * (1 - p.sharpness)) * (0.94 + 0.12 * r.random())
    drift = 1.0 + 0.05 * r.random() + 0.04 * p.glide
    freq = lerp(t, [(0, f0 * 0.94), (dur * 0.5, f0 * drift), (dur, f0 * 0.90)])
    env = bell(t, dur * (0.18 + 0.10 * (1 - p.sharpness)), dur * 0.30)
    soft = p.soft(breath=max(p.breath, 0.12) + 0.10, quackiness=p.quackiness * 0.25, am_rate=p.am_rate * 0.30,
                  vib_rate=p.vib_rate * 0.45, vib_depth=p.vib_depth * 0.7)
    return normalise(mul(voice(soft, t, freq, r, 1.0, 1.0), env), -5)


def tick():
    n = int(0.03 * RATE)
    return [0.7 * math.sin(TAU * 2000 * i / RATE) * math.exp(-i / (0.005 * RATE)) for i in range(n)]


def first_greet(p, double):
    """The first greet variant that is (or is not) a double quack."""
    for v in range(12):
        sig, d = greet(p, v)
        if d == double:
            return sig
    return greet(p, 0)[0]


def sounds(seed):
    p = Personality(seed)
    gap = [0.0] * int(0.06 * RATE)
    return {
        'quack': first_greet(p, False),            # woken
        'quack_double': first_greet(p, True),      # a command done
        'quack_happy': chirp(p, 2),                # a skill done: the trill
        'quack_question': inquire(p, 0),           # nothing heard
        'quack_angry': peck(p, 0) + gap + peck(p, 1),  # refused, stopped
        'quack_scared': alarm(p, 0),               # fallen
        'quack_sad': coo(p, 0),                    # battery low
        'quack_sleepy': coo(p, 1),                 # nobody around
        'tick': tick(),
    }


def write(path, samples):
    data = b''.join(struct.pack('<h', max(-32768, min(32767, int(v * 32767)))) for v in samples)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data)


def main():
    args = sys.argv[1:]
    seed = 146
    if '--seed' in args:
        i = args.index('--seed')
        seed = int(args[i + 1])
        del args[i:i + 2]
    out = args[0] if args else os.path.join(os.path.dirname(__file__), '..', 'sounds')
    os.makedirs(out, exist_ok=True)
    p = Personality(seed)
    print(f'seed {seed}: pitch {p.pitch:.0f} Hz, quackiness {p.quackiness:.2f}, speed {p.speed:.2f}')
    for name, samples in sounds(seed).items():
        write(os.path.join(out, name + '.wav'), samples)
        print(f'{name}.wav  {len(samples) / RATE:.2f} s')


if __name__ == '__main__':
    main()
