#!/usr/bin/env python3
"""Checks build/vru-render-patch on real patches: PNG counts, non-black pictures, byte-identical repeats,
the seed, and the report. Run by `make test`. Usage: tests/render_patch_test.py [path/to/vru-render-patch]"""
import glob, os, shutil, struct, subprocess, sys, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "vru-render-patch")
OUT = os.path.join(ROOT, "build", "render-patch-test")
SIZE = "96x72"
fails = 0
checks = 0


def check(ok, msg):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        print("FAIL: " + msg)


def run(patch, out, seed=1, frames=3, seconds=1):
    shutil.rmtree(out, ignore_errors=True)
    return subprocess.run([TOOL, patch, "--out", out, "--seconds", str(seconds), "--frames", str(frames), "--size", SIZE,
                           "--seed", str(seed), "--fps", "20"], capture_output=True, text=True)


def pngs(out):
    return sorted(glob.glob(os.path.join(out, "*.png")))


def lit_fraction(path):
    """Fraction of pixels with any channel above 24, for an 8-bit RGB, non-interlaced PNG."""
    data = open(path, "rb").read()
    check(data[:8] == b"\x89PNG\r\n\x1a\n", path + " is a PNG")
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 3
    prev = bytearray(stride)
    lit = 0
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        for x in range(w):
            if max(line[x * 3:x * 3 + 3]) > 24:
                lit += 1
        prev = line
    return lit / float(w * h)


def same_files(a, b):
    fa, fb = pngs(a), pngs(b)
    if [os.path.basename(x) for x in fa] != [os.path.basename(x) for x in fb]:
        return False
    return all(open(x, "rb").read() == open(y, "rb").read() for x, y in zip(fa, fb))


if not os.path.exists(TOOL):
    print("missing " + TOOL)
    sys.exit(1)

# The 2.2.0 test patch: three Displays, each with a picture.
patch = os.path.join(ROOT, "patches", "VRU-2.2.0-test.vcv")
r = run(patch, OUT + "/a")
check(r.returncode == 0, "2.2.0 test patch renders: " + r.stderr[-200:])
check(len(pngs(OUT + "/a")) == 9, "2.2.0 test patch: 3 Displays x 3 frames = 9 PNGs, got %d" % len(pngs(OUT + "/a")))
for p in pngs(OUT + "/a"):
    check(lit_fraction(p) > 0.01, "not black: " + os.path.basename(p))
check("Fundamental/LFO #5: stand-in" in r.stdout, "report lists the Fundamental LFO stand-in")
check("0.125 Hz" in r.stdout, "report gives the LFO's saved frequency")
check("real module" in r.stdout, "report says which modules are real")
run(patch, OUT + "/b")
check(same_files(OUT + "/a", OUT + "/b"), "2.2.0 test patch: same options give byte-identical PNGs")
check(open(OUT + "/a/report.txt").read() == r.stdout, "report.txt matches what is printed")
f6 = [p for p in pngs(OUT + "/a") if "display-6-" in p]
check(len(f6) == 3 and open(f6[0], "rb").read() != open(f6[2], "rb").read(), "frames at different times differ")

# The 2.3.0 test patch: five rows, each ending in its own Display. Row 1 (Display 5) needs the Video Probe's outputs, which are
# 0 V offline, so it is only checked for a picture. Rows 2 to 5 (Displays 13, 17, 24, 30) must be lit, not blown out, and moving.
t230 = os.path.join(ROOT, "patches", "VRU-2.3.0-test.vcv")
ta = run(t230, OUT + "/t230a", frames=4, seconds=3)
run(t230, OUT + "/t230b", frames=4, seconds=3)
check(ta.returncode == 0, "2.3.0 test patch renders: " + ta.stderr[-200:])
check(len(pngs(OUT + "/t230a")) == 20, "2.3.0 test patch: 5 Displays x 4 frames = 20 PNGs, got %d" % len(pngs(OUT + "/t230a")))
check("VideoResearchUnit/VideoProbe #3: real module" in ta.stdout, "2.3.0 test patch: the report lists the Video Probe")
check("VideoResearchUnit/FrameDelay #9: real module" in ta.stdout, "2.3.0 test patch: the report lists the Frame Delay")
check("VideoResearchUnit/Feedback #29: real module" in ta.stdout, "2.3.0 test patch: the report lists Feedback")
check(same_files(OUT + "/t230a", OUT + "/t230b"), "2.3.0 test patch: byte-identical on repeat")
for did in (5, 13, 17, 24, 30):
    fr = [p for p in pngs(OUT + "/t230a") if "display-%d-" % did in p]
    check(len(fr) == 4, "2.3.0 test patch: Display %d has 4 frames, got %d" % (did, len(fr)))
    fl = [lit_fraction(p) for p in fr]
    # Rows 3 and 4 fill the picture on purpose; the shape row and the dot row must stay mostly black.
    top = 0.5 if did in (13, 30) else 1.01
    check(all(0.005 < f < top for f in fl), "2.3.0 test patch: Display %d lit but not blown out, got %s" % (did, ", ".join("%.3f" % f for f in fl)))
    if did != 5:
        check(all(open(fr[i], "rb").read() != open(fr[i + 1], "rb").read() for i in range(len(fr) - 1)),
              "2.3.0 test patch: Display %d changes between frames" % did)

# Two numbered demos.
for name, count in (("2 - Video Oscillator Moire.vcv", 3), ("5 - Octoscillator Garden.vcv", 3)):
    tag = name.split(" ")[0]
    pa = run(os.path.join(ROOT, "patches", name), OUT + "/d" + tag + "a")
    run(os.path.join(ROOT, "patches", name), OUT + "/d" + tag + "b")
    check(pa.returncode == 0, name + " renders")
    check(len(pngs(OUT + "/d" + tag + "a")) == count, name + ": %d PNGs, got %d" % (count, len(pngs(OUT + "/d" + tag + "a"))))
    for p in pngs(OUT + "/d" + tag + "a"):
        check(lit_fraction(p) > 0.01, name + " not black: " + os.path.basename(p))
    check(same_files(OUT + "/d" + tag + "a", OUT + "/d" + tag + "b"), name + ": byte-identical on repeat")
    if tag == "2":
        check("Fundamental/LFO" in pa.stdout and "stand-in" in pa.stdout, name + ": report lists the LFO stand-in")

# The loop through a Frame Delay (patch 7): the trail must keep changing, and neither die to black nor fill the picture.
loop = "7 - Loop Through A Delay.vcv"
la = run(os.path.join(ROOT, "patches", loop), OUT + "/l7a", frames=4, seconds=3)
run(os.path.join(ROOT, "patches", loop), OUT + "/l7b", frames=4, seconds=3)
check(la.returncode == 0, loop + " renders: " + la.stderr[-200:])
check("VideoResearchUnit/FrameDelay #4: real module" in la.stdout, loop + ": the report says the Frame Delay is a real module")
lp = pngs(OUT + "/l7a")
check(len(lp) == 4, loop + ": 4 PNGs, got %d" % len(lp))
fractions = [lit_fraction(p) for p in lp]
check(all(0.005 < f < 0.5 for f in fractions), loop + ": every frame is lit but not blown out, got " + ", ".join("%.3f" % f for f in fractions))
check(all(open(lp[i], "rb").read() != open(lp[i + 1], "rb").read() for i in range(len(lp) - 1)), loop + ": each frame differs from the one before")
check(same_files(OUT + "/l7a", OUT + "/l7b"), loop + ": byte-identical on repeat")

# One oscillator bends another (patch 8): oscillator 1's picture on oscillator 2's FM IN. Lit, changing, repeatable.
bend = "8 - One Oscillator Bends Another.vcv"
ba = run(os.path.join(ROOT, "patches", bend), OUT + "/b8a", frames=4, seconds=3)
run(os.path.join(ROOT, "patches", bend), OUT + "/b8b", frames=4, seconds=3)
check(ba.returncode == 0, bend + " renders: " + ba.stderr[-200:])
check("VideoResearchUnit/VideoOsc #2: real module" in ba.stdout, bend + ": the report says oscillator 2 is a real module")
bp = pngs(OUT + "/b8a")
check(len(bp) == 4, bend + ": 4 PNGs, got %d" % len(bp))
check(all(lit_fraction(p) > 0.05 for p in bp), bend + ": every frame is lit")
check(all(open(bp[i], "rb").read() != open(bp[i + 1], "rb").read() for i in range(len(bp) - 1)), bend + ": the picture moves between frames")
check(same_files(OUT + "/b8a", OUT + "/b8b"), bend + ": byte-identical on repeat")

# Noise: real per-sample noise, seeded. Same seed repeats, another seed differs. An unknown plugin is reported.
noise = os.path.join(ROOT, "tests", "data", "noise-scope.vcv")
na = run(noise, OUT + "/na", seed=7)
run(noise, OUT + "/nb", seed=7)
run(noise, OUT + "/nc", seed=8)
check(na.returncode == 0 and len(pngs(OUT + "/na")) == 3, "noise patch renders 3 PNGs")
check(same_files(OUT + "/na", OUT + "/nb"), "noise: same seed is byte-identical")
check(not same_files(OUT + "/na", OUT + "/nc"), "noise: another seed changes the picture")
check("Fundamental/Noise #1: stand-in" in na.stdout and "every sample" in na.stdout, "report lists the Noise stand-in")
check("Mystery/Widget #4: unsupported, outputs 0 V" in na.stdout, "report lists an unsupported module")
for p in pngs(OUT + "/na"):
    check(lit_fraction(p) > 0.002, "noise picture is not black: " + os.path.basename(p))

# Video Player and Syphon In draw a labelled test card.
muy = run(os.path.join(ROOT, "patches", "6 - Muybridge on the Scan Processor.vcv"), OUT + "/m", frames=1)
check("VideoPlayer #1: no source offline, draws a labelled test card" in muy.stdout, "report lists the Video Player test card")
check(len(pngs(OUT + "/m")) == 1 and lit_fraction(pngs(OUT + "/m")[0]) > 0.05, "Video Player test card is not black")

# Bad input.
bad = subprocess.run([TOOL, "/nonexistent.vcv", "--out", OUT + "/x"], capture_output=True, text=True)
check(bad.returncode != 0, "a missing patch is an error")

print("render_patch_test: %d checks, %d failures" % (checks, fails))
sys.exit(1 if fails else 0)
