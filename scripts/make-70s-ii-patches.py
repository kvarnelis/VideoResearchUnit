#!/usr/bin/env python3
"""Write five dense 1970s-video-art studio patches (Video Research Unit 2.3.0), "70s II ...".

Each is 16 to 20 modules in three rows by signal flow (sources and mixing / processing / modulation, Notes and
Display), with several slow CV sources at unrelated rates (Octoscillator, Dual LFO Chaos) and a Video Probe that
reads the finished picture back as CV so the patch steers itself. Output is a zstd tar like Rack saves.
Uses the Patch class and helpers from make-70s-patches.py (same folder). Set VRU_SRC to a checkout with current res/.

Usage: scripts/make-70s-ii-patches.py [output-dir] [name ...]"""
import importlib.util, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("m70", os.path.join(HERE, "make-70s-patches.py"))
m70 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(m70)
Patch, VRU, FUND = m70.Patch, m70.VRU, m70.FUND
RED, YEL, GREEN, BLUE, PURPLE = m70.RED, m70.YEL, m70.GREEN, m70.BLUE, m70.PURPLE
hz_norm, yoke_rate, OUT = m70.hz_norm, m70.yoke_rate, m70.OUT

# Octoscillator output index: oscillator i (0-7), axis c (0 X, 1 Y, 2 Z)
def oc(i, c=0):
    return 3 * i + c

RATES = [-1.2, 0.4, -0.7, 1.1, -1.7, 0.2, 0.9, -0.4]     # unrelated rate multipliers (log2), one per oscillator


def octo(p, row, speed=-3.0, depths=None, relate=0.15):
    params = {0: speed, 2: relate, 3: 1.0}
    for i in range(8):
        params[5 + i] = RATES[i]
        params[13 + i] = (depths or {}).get(i, 0.3)
    return p.add(VRU, "Octoscillator", row, params)


def chaos(p, row, a=0.05, b=0.031, depth=0.3):
    return p.add(VRU, "DualLfoChaos", row, {0: hz_norm(a), 1: depth, 2: hz_norm(b), 3: depth, 4: 0.2, 5: 0.5})


def probe(p, row, src, **kw):
    pr = p.add(VRU, "VideoProbe", row, {2: 0.6, 3: 0.5})
    p.cable(src, 0, pr, 0, BLUE)
    return pr


def chain(p, ids, color=PURPLE):
    for a, b in zip(ids, ids[1:]):
        p.cable(a, 0, b, 0, color)


# -------------------------------------------------------------------------------------------------
# I. Scan landscape (Vasulka, Rutt-Etra): two reliefs of one terrain, one raster and one line field.
def scan_landscape():
    p = Patch()
    sh = p.add(VRU, "Shapes", 0, {0: 8, 1: 1.0, 3: 0.4, 12: 0.7, 13: 0})
    ra = p.add(VRU, "Ramps", 0, {0: 1, 1: 0.35, 4: 0, 5: 0.6, 8: 1})
    m1 = p.add(VRU, "BusMixer", 0, {0: 0.5, 1: 1})
    ws = p.add(VRU, "WaveShaper", 0, {0: 1.2, 2: 0.3, 4: 0.3, 7: 1.0})
    vo = p.add(VRU, "VideoOsc", 0, {0: math.log2(2 * 4.0), 3: 0.0, 4: 6, 10: 0, 12: 0.9, 15: 0})
    m2 = p.add(VRU, "BusMixer", 0, {0: 0.3, 1: 0})
    sp = p.add(VRU, "ScanProcessor", 1, {0: 0.3, 4: 1.0, 5: 2, 6: 0.4, 8: 0, 9: 0.3, 10: 0.15, 11: 0.08})
    ro = p.add(VRU, "Rotator", 1, {0: 0.0, 1: 0.01, 7: 2})
    fb = p.add(VRU, "Feedback", 1, {0: 0.0, 1: 0.01, 4: 0.03, 5: 0.2, 8: 2, 9: 0})
    lf = p.add(VRU, "LineField", 1, {0: 0.5, 5: 60, 6: 0.8, 7: 1.0})
    m3 = p.add(VRU, "BusMixer", 1, {0: 0.3, 1: 1})
    co = p.add(VRU, "Colorizer", 1, {0: 0.6, 1: 0.5, 2: 0.08, 3: 0.14, 4: 1, 5: 1, 6: 1, 7: 1, 8: 0.9, 9: 0})
    cl = p.add(VRU, "ColorLab", 1, {1: 1.0, 3: 1.5})
    fo = p.add(VRU, "Focus", 1, {0: 0.1})
    oc_ = octo(p, 2, -3.5, {0: 0.35, 1: 0.35, 2: 0.2, 3: 0.2, 4: 0.3, 5: 0.15})
    ch = chaos(p, 2, 0.05, 0.031, 0.2)
    dp = p.add(VRU, "Display", 2)
    pr = probe(p, 2, fo)
    av = p.add(FUND, "8vert", 2, {0: 0.04})
    p.notes(2, "Raster relief in the manner of the Vasulkas' Rutt-Etra work. One terrain (a soft dome, ramps and rolling rows, folded by the Wave Shaper) "
            "is lifted twice: once as a green phosphor raster by the Scan Processor, once as a field of lines by the Line Field, and the two are mixed. "
            "The raster also feeds a faint Feedback echo and a slow Rotator.\n"
            "Octoscillator moves the dome and tilts the lift at unrelated rates; Dual LFO Chaos drifts the oscillator rows. "
            "The Video Probe reads the finished picture's average brightness and, through an attenuator, steers the Scan Processor luma gain, so a bright picture settles itself.\n"
            "Play: Scan Processor LUMA GAIN, the Line Field DEPTH, the final BusMixer MIX.")
    chain(p, [m1, ws, m2, sp, ro, fb, m3, co, cl, fo, dp])
    p.cable(sh, 0, m1, 0); p.cable(ra, 0, m1, 1); p.cable(vo, 0, m2, 1)
    p.cable(m2, 0, lf, 0); p.cable(lf, 0, m3, 1)
    p.cable(oc_, oc(0), sh, 4, GREEN); p.cable(oc_, oc(1), sh, 5, GREEN)
    p.cable(oc_, oc(2), sp, 6, GREEN); p.cable(oc_, oc(3), sp, 7, GREEN)
    p.cable(oc_, oc(4), ro, 1, GREEN); p.cable(oc_, oc(5), fb, 2, GREEN)
    p.cable(ch, 0, vo, 0, YEL); p.cable(ch, 1, ra, 1, YEL)
    p.cable(pr, 4, av, 0, YEL); p.cable(av, 0, sp, 1, YEL)
    return p


# -------------------------------------------------------------------------------------------------
# II. Feedback cathedral (Skip Sweeney, early Vasulka): full-frame, two feedback stages, two rotators, an outer loop.
def feedback_cathedral():
    p = Patch()
    sh = p.add(VRU, "Shapes", 0, {0: 6, 1: 0.4, 3: 0.32, 4: 0.12, 8: 1, 13: 1})
    ra = p.add(VRU, "Ramps", 0, {0: 2, 1: 0.5, 4: 3, 5: 0.3, 8: 3})
    vo = p.add(VRU, "VideoOsc", 0, {0: math.log2(2 * 3.0), 4: 6, 10: 1, 12: 0.9, 15: 0})
    vm = p.add(VRU, "VideoMixer", 0, {0: 0.02, 1: 0.55, 2: 0.8, 4: 1, 5: 1, 6: 1, 8: 1.0, 25: 0.5, 26: 0.3, 27: 0.15, 28: 0.05, 29: 1})
    ws = p.add(VRU, "WaveShaper", 0, {0: 1.0, 4: 0.3, 7: 0.6})
    f1 = p.add(VRU, "Feedback", 1, {0: 0.05, 1: 0.02, 4: 0.1, 5: 0.88, 8: 1, 9: 0})
    r1 = p.add(VRU, "Rotator", 1, {0: 0.0, 1: 0.02, 7: 3, 8: 3})
    f2 = p.add(VRU, "Feedback", 1, {0: 0.03, 1: -0.015, 4: 0.09, 5: 0.4, 8: 1, 9: 1})
    r2 = p.add(VRU, "Rotator", 1, {0: 0.0, 1: -0.01, 7: 3, 8: 6})
    fo = p.add(VRU, "Focus", 1, {0: 0.1, 1: 0.2})
    fd = p.add(VRU, "FrameDelay", 1, {0: 2.0})
    cl = p.add(VRU, "ColorLab", 1, {1: 1.5, 3: 1.6})
    oc_ = octo(p, 2, -3.5, {0: 0.35, 1: 0.35, 2: 0.12, 3: 0.12, 4: 0.2, 5: 0.15, 6: 0.3})
    ch = chaos(p, 2, 0.045, 0.027, 0.15)
    dp = p.add(VRU, "Display", 2)
    pr = probe(p, 2, cl)
    av = p.add(FUND, "8vert", 2, {0: -0.3, 1: 0.03})
    p.notes(2, "Camera-at-the-monitor feedback in the manner of Skip Sweeney and the early Vasulka studies, filling the whole frame. "
            "A drifting shape, rings and bars are tinted and layered in the Video Mixer, folded by the Wave Shaper and sent round two Feedback stages with a Rotator after each "
            "(the second folds the picture into a six-fold mandala), then softened by Focus, held two frames back by the Frame Delay and given contrast by the Color Lab.\n"
            "Octoscillator and Dual LFO Chaos breathe the zoom, turn and hue at unrelated slow rates. The Video Probe measures the finished picture and, through an attenuator, pulls the first Feedback GAIN down when the picture gets bright, so it does not flood.\n"
            "Play: first Feedback GAIN, the Rotators' FOLDS, the Video Mixer LEVELs. Press CLEAR after a big change.")
    chain(p, [vm, f1, r1, f2, r2, ws, fo, fd, cl, dp])
    p.cable(sh, 0, vm, 0); p.cable(ra, 0, vm, 1); p.cable(vo, 0, vm, 2)
    p.cable(fd, 0, vm, 3)                       # outer loop: the delayed picture comes back at low level
    p.cable(oc_, oc(0), sh, 4, GREEN); p.cable(oc_, oc(1), sh, 5, GREEN)
    p.cable(oc_, oc(2), f1, 1, GREEN); p.cable(oc_, oc(3), f1, 2, GREEN)
    p.cable(oc_, oc(4), f2, 5, GREEN); p.cable(oc_, oc(5), r2, 1, GREEN)
    p.cable(ch, 0, f1, 5, YEL); p.cable(ch, 1, f2, 2, YEL)
    p.cable(pr, 4, av, 0, YEL); p.cable(av, 0, f1, 6, YEL)
    return p


# -------------------------------------------------------------------------------------------------
# III. Keyed colour (Sandin Image Processor, Paik-Abe): three layers keyed against each other, plus a vector line layer.
def keyed_colour():
    p = Patch()
    ra = p.add(VRU, "Ramps", 0, {0: 2, 1: 0.55, 4: 0, 5: 0.9, 8: 3})
    ws = p.add(VRU, "WaveShaper", 0, {0: 1.4, 2: 0.55, 3: 0.35})
    c1 = p.add(VRU, "Colorizer", 0, {0: 0.0, 1: 0.3, 2: 0.6, 3: 0.85, 4: 1.0, 5: 1.0, 6: 1.0, 7: 1.0, 8: 1.0, 9: 0})
    vo = p.add(VRU, "VideoOsc", 0, {4: 1})
    cl = p.add(VRU, "ColorLab", 0, {0: 0.0, 1: 1.4, 4: 0.3})
    sh = p.add(VRU, "Shapes", 0, {0: 6, 1: 0.2, 3: 0.45, 8: 1, 9: 5, 10: 0.5, 13: 1})
    vm = p.add(VRU, "VideoMixer", 1, {17: 1, 18: 0.5, 19: 0.06, 22: 1})
    ky = p.add(VRU, "Keyer", 1, {0: 0.5, 1: 0.3, 3: 1, 5: 0.1, 6: 0.55, 7: 0.3})
    vx = p.add(VRU, "VectorOsc", 1)
    st = p.add(VRU, "ScopeTrace", 1, {7: 0.5, 9: 0.8})
    bm = p.add(VRU, "BusMixer", 1, {0: 0.5, 1: 3})
    fb = p.add(VRU, "Feedback", 1, {0: 0.03, 1: 0.01, 4: 0.05, 5: 0.35, 8: 2, 9: 0})
    c2 = p.add(VRU, "ColorLab", 1, {1: 1.3, 3: 1.4, 5: 0.15})
    fo = p.add(VRU, "Focus", 1, {0: 0.05})
    oc_ = octo(p, 2, -3.5, {0: 0.4, 1: 0.4, 2: 0.3, 3: 0.2, 4: 0.3})
    ch = chaos(p, 2, 0.05, 0.031, 0.2)
    dp = p.add(VRU, "Display", 2)
    pr = probe(p, 2, fo)
    av = p.add(FUND, "8vert", 2, {0: 0.03})
    p.notes(2, "Colour and keying in the manner of the Sandin Image Processor and the Paik-Abe synthesizer. Three layers: rings folded by the Wave Shaper and painted by the Colorizer; "
            "coloured bars from a Video Oscillator; and a polygon ring from Shapes that acts as the matte. The Video Mixer cuts the polygon out of the rings and shows the bars through it; "
            "the Keyer then keys the result again, its threshold pushed around pixel by pixel by the bars. A Vector Oscillator drawn on the Scope Trace adds a glowing line layer "
            "that is screened over the top, and a light Feedback leaves an echo.\n"
            "Octoscillator drifts the matte and the vector figure; Dual LFO Chaos shifts the key hue. The Video Probe measures the picture and steers the Keyer threshold through an attenuator.\n"
            "Play: Video Mixer THRESH, Keyer THRESH, Colorizer BANDS.")
    chain(p, [ra, ws, c1, vm, ky, bm, fb, c2, fo, dp])
    p.cable(vo, 0, cl, 0); p.cable(cl, 0, vm, 5); p.cable(sh, 0, vm, 6)
    p.cable(vo, 0, ky, 5)
    p.cable(vx, 0, st, 0, GREEN); p.cable(vx, 1, st, 1, GREEN); p.cable(vx, 2, st, 2, GREEN)
    p.cable(st, 0, bm, 1)
    p.cable(oc_, oc(0), sh, 4, GREEN); p.cable(oc_, oc(1), sh, 5, GREEN)
    p.cable(oc_, oc(2), vx, 2, GREEN); p.cable(oc_, oc(3), vx, 3, GREEN)
    p.cable(oc_, oc(4), fb, 2, GREEN)
    p.cable(ch, 0, vo, 1, YEL)
    p.cable(pr, 4, av, 0, YEL); p.cable(av, 0, ky, 1, YEL)
    return p


# -------------------------------------------------------------------------------------------------
# IV. Direct video (Stephen Beck): no camera; oscillators, ramps and shapes only, additively tinted, frozen and differenced.
def direct_video_ii():
    p = Patch()
    v1 = p.add(VRU, "VideoOsc", 0, {0: math.log2(2 * 5.0), 1: 0.1, 2: 0.0, 3: 0.2, 4: 6, 10: 1, 12: 0.85, 15: 0})
    v2 = p.add(VRU, "VideoOsc", 0, {0: math.log2(2 * 7.0), 1: 0.5, 3: 0.0, 4: 6, 10: 0, 12: 0.85, 15: 0})
    ra = p.add(VRU, "Ramps", 0, {0: 2, 1: 0.6, 4: 3, 5: 0.3, 8: 3})
    sh = p.add(VRU, "Shapes", 0, {0: 4, 1: 0.1, 3: 0.4, 8: 1, 9: 6, 10: 0.55, 13: 1})
    vm = p.add(VRU, "VideoMixer", 0, {0: 0.0, 1: 0.33, 2: 0.66, 3: 0.1, 4: 1, 5: 1, 6: 1, 7: 1, 8: 1.0, 25: 1.0, 26: 1.0, 27: 1.0, 28: 0.5, 29: 1})
    ff = p.add(VRU, "FreezeFrame", 1, {1: 0.5, 2: 0.3})
    bm = p.add(VRU, "BusMixer", 1, {0: 0.5, 1: 5})
    ws = p.add(VRU, "WaveShaper", 1, {0: 1.0, 3: 0.2, 4: 0.4, 7: 0.7})
    ky = p.add(VRU, "Keyer", 1, {0: 0.4, 1: 0.3, 3: 2, 6: 0.5})
    ro = p.add(VRU, "Rotator", 1, {0: 0.0, 1: 0.01, 7: 3, 8: 2})
    co = p.add(VRU, "Colorizer", 1, {0: 0.0, 1: 0.33, 2: 0.66, 3: 0.9, 8: 0.9, 9: 0})
    b2 = p.add(VRU, "BusMixer", 1, {0: 0.45, 1: 0})
    fo = p.add(VRU, "Focus", 1, {0: 0.05, 3: 0.3})
    cl = p.add(VRU, "ColorLab", 1, {1: 1.8, 3: 1.5})
    oc_ = octo(p, 2, -3.5, {0: 0.4, 1: 0.4, 2: 0.3, 3: 0.3, 4: 0.3, 5: 0.2, 6: 0.3})
    ch = chaos(p, 2, 0.06, 0.04, 0.25)
    dp = p.add(VRU, "Display", 2)
    pr = probe(p, 2, fo)
    av = p.add(FUND, "8vert", 2, {0: 0.05})
    p.notes(2, "Direct video: no camera, in the manner of Stephen Beck's Direct Video Synthesizer. Two Video Oscillators (bars and rows), rings and rays from Ramps and a ring of Shapes "
            "are tinted red, green, blue and mixed additively in the Video Mixer. A Freeze Frame captures the picture every few seconds and the Bus Mixer shows the difference between now and then, "
            "so only what moved glows. The Wave Shaper quantizes it into flat colour, the Keyer cuts it, the Rotator folds it and the Colorizer repaints it.\n"
            "Octoscillator moves the shapes and the oscillator hues and rates; Dual LFO Chaos moves the Freeze Frame strobe. The Video Probe reads the picture and, through an attenuator, steers the Colorizer offset.\n"
            "Play: Video Mixer LEVELs, Freeze Frame STROBE, Wave Shaper QUANT.")
    p.cable(v1, 0, vm, 0); p.cable(v2, 0, vm, 1); p.cable(ra, 0, vm, 2); p.cable(sh, 0, vm, 3)
    p.cable(vm, 0, ff, 0); p.cable(vm, 0, bm, 0); p.cable(ff, 0, bm, 1)
    chain(p, [bm, ws, ky, ro, co, b2, fo, cl, dp])
    p.cable(ro, 0, b2, 1)
    p.cable(oc_, oc(0), sh, 4, GREEN); p.cable(oc_, oc(1), sh, 5, GREEN)
    p.cable(oc_, oc(2), v1, 1, GREEN); p.cable(oc_, oc(3), v2, 1, GREEN)
    p.cable(oc_, oc(4), ro, 1, GREEN); p.cable(oc_, oc(5), v1, 0, GREEN)
    p.cable(ch, 0, ff, 4, YEL)
    p.cable(pr, 4, av, 0, YEL); p.cable(av, 0, co, 7, YEL)
    return p


# -------------------------------------------------------------------------------------------------
# V. Wobbulator (Paik, Abe): colour bars, a grid and a face of shapes bent by two yokes, with a scope Lissajous over them.
def wobbulator_ii():
    p = Patch()
    v1 = p.add(VRU, "VideoOsc", 0, {4: 1})
    v2 = p.add(VRU, "VideoOsc", 0, {0: math.log2(2 * 3.0), 4: 5, 12: 0.9, 15: 0})
    sh = p.add(VRU, "Shapes", 0, {0: 5, 1: 0.5, 3: 0.5, 8: 1, 9: 3, 10: 0.4, 13: 1})
    vm = p.add(VRU, "VideoMixer", 0, {0: 0.0, 1: 0.33, 2: 0.66, 4: 0, 5: 1, 6: 1, 8: 1.0, 25: 0.8, 26: 0.25, 27: 0.3, 29: 1})
    y1 = p.add(VRU, "DeflectionYoke", 1, {0: 0.8, 1: 0.35, 2: 0.7, 3: yoke_rate(120.6), 9: yoke_rate(61.7), 19: 0.5, 20: yoke_rate(91.3), 17: 0.15, 18: 0.15})
    y2 = p.add(VRU, "DeflectionYoke", 1, {0: 0.3, 1: 0.5, 2: 0.5, 3: yoke_rate(33.3), 9: yoke_rate(47.9), 5: 0.4, 6: 0.3, 17: 0.1, 18: 0.1})
    fb = p.add(VRU, "Feedback", 1, {0: 0.01, 1: 0.01, 4: 0.04, 5: 0.15, 8: 0, 9: 0})
    vx = p.add(VRU, "VectorOsc", 1)
    st = p.add(VRU, "ScopeTrace", 1, {7: 0.6, 9: 0.9, 8: 0.5})
    bm = p.add(VRU, "BusMixer", 1, {0: 0.35, 1: 1})
    cl = p.add(VRU, "ColorLab", 1, {1: 1.3})
    fo = p.add(VRU, "Focus", 1, {0: 0.05, 3: 0.3})
    oc_ = octo(p, 2, -3.5, {0: 0.3, 1: 0.3, 2: 0.3, 3: 0.3, 4: 0.3})
    ch = chaos(p, 2, 0.07, 0.05, 0.3)
    dp = p.add(VRU, "Display", 2)
    pr = probe(p, 2, fo)
    av = p.add(FUND, "8vert", 2, {0: 0.04})
    p.notes(2, "Wobbulator, after Nam June Paik's Magnet TV and the Paik-Abe synthesizer. SMPTE colour bars, a grid and a ring of Shapes are mixed, then bent by two Deflection Yokes in series, "
            "one with a sideways coil and a swirl coil, one with a slower, curling pair, so the picture ripples and winds. A little Feedback trails it, "
            "and a Vector Oscillator drawn on the Scope Trace is screened over the top as a glowing figure.\n"
            "Octoscillator and Dual LFO Chaos pull the coil amounts, the vector figure and the shapes at unrelated slow rates. The Video Probe measures how much the picture is changing and, "
            "through an attenuator, adds to the first yoke's depth, so a still picture gets pulled harder.\n"
            "Play: the first Yoke DEPTH, the H and S coil amounts, the second Yoke CURL.")
    p.cable(v1, 0, vm, 0); p.cable(v2, 0, vm, 1); p.cable(sh, 0, vm, 2)
    chain(p, [vm, y1, y2, fb, bm, cl, fo, dp])
    p.cable(vx, 0, st, 0, GREEN); p.cable(vx, 1, st, 1, GREEN); p.cable(vx, 2, st, 2, GREEN)
    p.cable(st, 0, bm, 1)
    p.cable(oc_, oc(0), y1, 11, GREEN); p.cable(oc_, oc(1), y1, 13, GREEN)
    p.cable(oc_, oc(2), y2, 18, GREEN); p.cable(oc_, oc(3), sh, 4, GREEN)
    p.cable(oc_, oc(4), vx, 2, GREEN)
    p.cable(ch, 0, y1, 4, YEL); p.cable(ch, 1, y2, 4, YEL)
    p.cable(pr, 5, av, 0, YEL); p.cable(av, 0, y1, 3, YEL)
    return p


PATCHES = [("70s II scan landscape.vcv", scan_landscape), ("70s II feedback cathedral.vcv", feedback_cathedral),
           ("70s II keyed colour.vcv", keyed_colour), ("70s II direct video.vcv", direct_video_ii),
           ("70s II wobbulator.vcv", wobbulator_ii)]

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    only = sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    for name, fn in PATCHES:
        if only and not any(o in name for o in only):
            continue
        print(m70.Patch.save(fn(), out, name))
