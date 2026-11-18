#!/usr/bin/env python3
"""Write five self-playing studio patches built on real footage ("70s III ..."), event-driven, not drifting.

Every patch is fed by a Video Player set to one fixed YouTube link; generators are only
secondary material (wipe patterns, mattes, polygons). Each patch carries the same kind of control section:
  - a master clock (Fundamental LFO square) whose tempo is pushed around by a slow macro LFO,
  - a SEQ3 phrase clocked by it: its step outputs are the bar divisions, its three rows are per-beat values,
  - a Random module clocked by the same beat whose trigger probability rises and falls with the macro (density),
  - ADSR hits fired by those beats and triggers (attack 0, so each is a strike with a decay),
  - Dual LFO Chaos as a counter-clock at an unrelated rate (its stepped output and its own clock pulse),
  - two macro LFOs at 150 s and 67 s that change the control section itself, so no 20 s ever repeats,
  - a Video Probe reading the finished picture and feeding it back into the controls,
  - one Fundamental 8vert as the attenuators.
Uses the Patch class and helpers from make-70s-patches.py (same folder). Set VRU_SRC to a checkout with current res/.

Usage: scripts/make-70s-iii-patches.py [output-dir] [name ...]"""
import importlib.util, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(fname, modname):
    spec = importlib.util.spec_from_file_location(modname, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


m70 = _load("make-70s-patches.py", "m70")
Patch, VRU, FUND = m70.Patch, m70.VRU, m70.FUND
RED, YEL, GREEN, BLUE, PURPLE = m70.RED, m70.YEL, m70.GREEN, m70.BLUE, m70.PURPLE
hz_norm, lfo_freq, OUT = m70.hz_norm, m70.lfo_freq, m70.OUT
WHITE = "#e8e8e8"

YOUTUBE = "https://www.youtube.com/watch?v=cVlu2AMGsZc"          # a public footage link

# the sphere source patch, stage by stage (param id: value)
SPHERE_WS = {0: 3.216, 1: 0.34, 2: 0.614, 3: 0.767, 4: 0.011, 5: 1.802, 6: 0.0, 7: 1.0}
SPHERE_FB = {0: 0.183, 1: 0.0, 2: -0.008, 3: 0.0, 4: 0.083, 5: 0.199, 6: 0.259, 7: 0.0, 8: 0.0, 9: 1.0}
SPHERE_RO = {2: 0.094, 7: 0, 8: 1}
SPHERE_SP = {0: 0.134, 1: -0.499, 2: 2.0, 3: 0.373, 4: 1.542, 5: 1, 6: 0.663, 7: 0, 8: 0, 9: 1.0, 10: 0.331, 11: 0.092}
SPHERE_CL = {0: 0.031, 1: 0.28, 2: 1.0, 3: 1.0, 4: 0.166, 8: 1.0}

# Fundamental output ids used here
SIN, TRI, SAW, SQR = 0, 1, 2, 3            # LFO
R_STEP, R_TRIG = 0, 4                      # Random: stepped value, trigger out
Q_TRIG, Q_ROW, Q_STEP = 0, 1, 4            # SEQ3: trigger out, row 1..3 = 1..3, step k gate = 4 + k
# Video Probe outputs
P_LUMA, P_AVG, P_MOTION, P_GATE = 0, 4, 5, 6


def player(p, row):
    return p.add(VRU, "VideoPlayer", row, {0: 0.0, 1: 0.0, 2: 0.0, 3: 1.0, 4: 0.4915665090084076},
                 data={"filePath": "", "source": "youtube", "youtubeUrl": YOUTUBE})


def chain(p, ids, color=PURPLE):
    for a, b in zip(ids, ids[1:]):
        p.cable(a, 0, b, 0, color)


def lfo(p, row, period, uni=True, fm=0.0, pw=0.5):
    """Fundamental LFO: OFFSET 1 = 0..10 V, FREQ log2 Hz, FM amount (param 3), PW (param 5)."""
    return p.add(FUND, "LFO", row, {0: 1.0 if uni else 0.0, 1: 0.0, 2: lfo_freq(period), 3: fm, 5: pw})


def seq3(p, row, rows, gates=None, steps=8):
    """SEQ3 played by an external clock: rows are three lists of eight values (0..10 V)."""
    params = {0: 1.0, 3: float(steps)}
    for r, vals in enumerate(rows):
        for s, v in enumerate(vals):
            params[4 + 8 * r + s] = float(v)
    g = gates or [1] * 8
    return p.add(FUND, "SEQ3", row, params, data={"running": True, "gates": [1 if x else 0 for x in g]})


def rnd(p, row, prob, prob_cv):
    """Fundamental Random fired by its TRIGGER input; PROB plus PROB CV x (PROB in / 10) decides each beat."""
    return p.add(FUND, "Random", row, {0: 0.0, 1: 0.0, 2: 1.0, 3: 1.0, 4: prob, 5: 1.0, 6: 0.0, 8: prob_cv})


def hit(p, row, decay, release, attack=0.0):
    """ADSR as a strike: attack (0 = instant), decay to sustain 0, release. Time = 1 ms x 10000^knob."""
    return p.add(FUND, "ADSR", row, {0: attack, 1: decay, 2: 0.0, 3: release})


def vert(p, row, gains):
    return p.add(FUND, "8vert", row, {i: g for i, g in enumerate(gains)})


def chaos(p, row, a_hz, b_hz=0.05, uni=True, slew=0.25):
    return p.add(VRU, "DualLfoChaos", row, {0: hz_norm(a_hz), 1: 1.0, 2: hz_norm(b_hz), 3: 1.0, 4: 0.45, 5: slew,
                                            6: 0.0 if uni else 1.0})


def probe(p, row, src, level=0.5):
    pr = p.add(VRU, "VideoProbe", row, {2: 0.6, 3: 0.5, 4: level})
    p.cable(src, 0, pr, 0, BLUE)
    return pr


def conductor(p, row, clock_s, clock_fm, rows, gates=None, prob=0.12, prob_cv=0.6, density=True):
    """The shared control section: clock, two macros, phrase, density. Returns the module ids."""
    k = {}
    k["clk"] = lfo(p, row, clock_s, fm=clock_fm)
    k["mA"] = lfo(p, row, 150.0)                       # macro A: tempo and density, 150 s
    k["mB"] = lfo(p, row, 67.0)                        # macro B: the second layer, 67 s
    k["seq"] = seq3(p, row, rows, gates)
    k["rnd"] = rnd(p, row, prob, prob_cv)
    p.cable(k["clk"], SQR, k["seq"], 1, RED)           # beat -> SEQ3 CLOCK
    p.cable(k["clk"], SQR, k["rnd"], 2, RED)           # beat -> Random TRIGGER
    p.cable(k["mA"], SIN, k["clk"], 0, YEL)            # macro A -> clock FM (tempo)
    if density:
        p.cable(k["mA"], SIN, k["rnd"], 4, YEL)        # macro A -> Random PROB (density)
    return k


# -------------------------------------------------------------------------------------------------
# 1. The sphere struck, turned and re-lit.
def struck_sphere():
    p = Patch()
    pl = player(p, 0)
    ws = p.add(VRU, "WaveShaper", 0, dict(SPHERE_WS))
    fb = p.add(VRU, "Feedback", 0, dict(SPHERE_FB))
    ro = p.add(VRU, "Rotator", 0, {**SPHERE_RO, 9: 2, 10: 0.7})          # notch 1/8 turn, 0.7 s glide
    sp = p.add(VRU, "ScanProcessor", 0, dict(SPHERE_SP))
    cl = p.add(VRU, "ColorLab", 0, dict(SPHERE_CL))
    dp = p.add(VRU, "Display", 0)
    chain(p, [pl, ws, fb, ro, sp])
    p.cable(sp, 1, cl, 0)
    p.cable(cl, 0, dp, 0)
    k = conductor(p, 1, 0.8, 0.07,
                  [[0, 1.5, 0.3, 1.8, 0, 0.8, 1.2, 0.2],      # row 1 -> Scan Processor LUMA GAIN (relief height)
                   [0, 0, 1.5, 0, 3, 0, 0.8, 2],              # row 2 -> Feedback HUE
                   [0, 2, 0.5, 3, 1, 0, 2.5, 1]])             # row 3 -> Wave Shaper FOLD (contour count)
    e1 = hit(p, 1, 0.75, 0.75)                                # downbeat punch
    e2 = hit(p, 1, 0.7, 0.8)                                  # struck trails
    ch = chaos(p, 1, 0.07)
    vt = vert(p, 1, [0.35, 0.5, 0.8, 0.35, 0.4, 0, 0, 0])
    pr = probe(p, 1, cl)
    p.notes(2, "70s III STRUCK SPHERE\n"
            "The sphere footage (footage, Wave Shaper contours, Feedback echo, Rotator, Scan Processor relief, Color Lab), "
            "now played by a clock instead of left to drift.\n"
            "Every beat the SEQ3 phrase resets the relief height, the contour count and the echo hue. On the downbeat an "
            "envelope punches the feedback zoom. Random strikes (sparse at first, densest near 0:40 and again near 3:10, "
            "thinnest around 1:50) turn the sphere an eighth of a turn with a glide and flare the trails; every 14 s or so "
            "the chaos clock snaps it back home. The 67 s macro brings colour in and out; the 150 s macro also drags the tempo.\n"
            "The probe reads the picture's motion: a busy picture keys the echo away, a still one lets it pile up.\n"
            "Play: Random PROB, Rotator STEP and GLIDE, Scan Processor LUMA GAIN.")
    p.cable(k["seq"], Q_ROW + 0, sp, 1, GREEN)
    p.cable(k["seq"], Q_ROW + 1, fb, 5, GREEN)
    p.cable(k["seq"], Q_ROW + 2, ws, 3, GREEN)
    p.cable(k["seq"], Q_STEP + 0, e1, 4, RED)
    p.cable(k["rnd"], R_TRIG, e2, 4, RED)
    p.cable(k["rnd"], R_TRIG, ro, 6, RED)                     # strike -> Rotator NOTCH
    p.cable(ch, 5, ro, 7, RED)                                # counter-clock -> Rotator RESET
    p.cable(e1, 0, vt, 0, YEL); p.cable(vt, 0, fb, 1, YEL)    # punch -> Feedback ZOOM
    p.cable(e2, 0, vt, 1, YEL); p.cable(vt, 1, fb, 6, YEL)    # strike -> Feedback GAIN
    p.cable(k["mB"], TRI, vt, 2, YEL); p.cable(vt, 2, cl, 2, YEL)   # macro B -> Color Lab SAT
    p.cable(ch, 4, vt, 3, YEL); p.cable(vt, 3, ws, 2, YEL)    # stepped chaos -> Wave Shaper BIAS
    p.cable(pr, P_MOTION, vt, 4, BLUE); p.cable(vt, 4, fb, 7, BLUE)   # motion -> Feedback KEY
    return p


# -------------------------------------------------------------------------------------------------
# 2. Feedback architecture built over each bar, cut on the downbeat, rebuilt.
def feedback_architecture():
    p = Patch()
    pl = player(p, 0)
    sh = p.add(VRU, "Shapes", 0, {0: 3.0, 1: 0.0, 3: 0.35, 7: 0.0, 9: 3, 10: 0.45, 12: 0.04})
    vm = p.add(VRU, "VideoMixer", 0, {10: 0.0, 15: 0})                          # A footage, B polygons, dissolve
    fb = p.add(VRU, "Feedback", 0, {0: -0.2, 1: -0.05, 4: 0.03, 5: 0.5, 6: 0.08, 8: 0, 9: 1})
    ro = p.add(VRU, "Rotator", 0, {2: 0.05, 7: 3, 8: 6, 9: 1, 10: 0.3})        # six folds, notch 1/12 turn
    fd = p.add(VRU, "FrameDelay", 0, {0: 4.0, 1: 1.0})
    cl = p.add(VRU, "ColorLab", 0, {0: 0.1, 1: 1.1, 2: 1.0, 3: 1.25, 4: 0.15, 5: 0.25, 8: 1.0})
    dp = p.add(VRU, "Display", 0)
    p.cable(pl, 0, vm, 0); p.cable(sh, 0, vm, 1)
    chain(p, [vm, fb, ro, fd, cl, dp])
    k = conductor(p, 1, 0.6, 0.08,
                  [[0, 2, 4, 1, 3, 0, 5, 2],                  # row 1 -> Feedback ZOOM (tunnel in or out)
                   [0, 0.5, 1, 0.2, 0.8, 0, 1.2, 0.3],        # row 2 -> Feedback ROTATE (spiral)
                   [0, 1, 3, 5, 0.5, 4, 2, 8]])               # row 3 -> Shapes SIDES
    saw = lfo(p, 1, 5.0)                                      # the build: a ramp restarted every downbeat
    e1 = hit(p, 1, 0.7, 0.72)
    ch = chaos(p, 1, 0.11, 0.023, uni=False)
    vt = vert(p, 1, [0.45, 0.5, 0.7, 1.0, 0.5, 0, 0, 0])
    pr = probe(p, 1, cl)
    p.notes(2, "70s III FEEDBACK ARCHITECTURE\n"
            "Footage and a polygon pass through one Feedback loop, a six-fold Rotator and a Frame Delay.\n"
            "Each bar of eight beats the feedback gain ramps up and the tunnel builds; the downbeat clears it and the build "
            "starts again. Every beat the SEQ3 phrase sets the tunnel's zoom and spiral and the polygon's number of sides; "
            "mid-bar an envelope throws the polygon open. Random strikes notch the kaleidoscope round by a twelfth and "
            "re-time the echo. The 150 s macro moves the tempo, the density and how much the polygon takes over from the "
            "footage (footage first, polygons strongest near 0:40, footage again around 1:50); the 67 s macro turns the colour.\n"
            "The probe reads the brightness: when the picture whites out, the feedback key rises and thins it.\n"
            "Play: Feedback GAIN, Rotator FOLDS, Video Mixer FADER.")
    p.cable(k["seq"], Q_ROW + 0, fb, 1, GREEN)
    p.cable(k["seq"], Q_ROW + 1, fb, 2, GREEN)
    p.cable(k["seq"], Q_ROW + 2, sh, 0, GREEN)
    p.cable(k["seq"], Q_STEP + 0, fb, 8, RED)                 # downbeat -> Feedback CLEAR
    p.cable(k["seq"], Q_STEP + 0, saw, 2, RED)                # downbeat -> ramp RESET
    p.cable(k["seq"], Q_STEP + 4, e1, 4, RED)                 # mid-bar -> polygon envelope
    p.cable(k["rnd"], R_TRIG, ro, 6, RED)                     # strike -> Rotator NOTCH
    p.cable(k["rnd"], R_STEP, fd, 1, GREEN)                   # stepped -> Frame Delay FRAMES
    p.cable(saw, SAW, vt, 0, YEL); p.cable(vt, 0, fb, 6, YEL)       # ramp -> Feedback GAIN
    p.cable(e1, 0, vt, 1, YEL); p.cable(vt, 1, sh, 3, YEL)          # envelope -> Shapes SIZE
    p.cable(k["mA"], SIN, vt, 2, YEL); p.cable(vt, 2, vm, 7, YEL)   # macro A -> Mixer FADER
    p.cable(k["mB"], TRI, vt, 3, YEL); p.cable(vt, 3, cl, 1, YEL)   # macro B -> Color Lab HUE
    p.cable(ch, 0, sh, 7, YEL)                                      # chaos A -> Shapes TURN
    p.cable(ch, 1, ro, 1, YEL)                                      # chaos B -> Rotator ANGLE
    p.cable(pr, P_AVG, vt, 4, BLUE); p.cable(vt, 4, fb, 7, BLUE)    # brightness -> Feedback KEY
    return p


# -------------------------------------------------------------------------------------------------
# 3. Global Groove: a keyed collage of the footage against delayed, frozen and contoured copies.
def global_groove():
    p = Patch()
    pl = player(p, 0)
    fb = p.add(VRU, "Feedback", 0, {0: -0.32, 1: 0.03, 2: 0.14, 3: -0.05, 4: 0.07, 5: 0.93, 6: 0.0, 8: 2, 9: 0})  # receding echoes
    fd = p.add(VRU, "FrameDelay", 0, {0: 12.0, 1: 1.0})
    cz = p.add(VRU, "Colorizer", 0, {0: 0.0, 1: 0.18, 2: 0.55, 3: 0.8, 4: 1, 5: 1, 6: 1, 7: 1, 8: 1.0, 9: 4, 10: 0.0})
    ff = p.add(VRU, "FreezeFrame", 0, {0: 1.0, 1: 1.0, 2: 0.0})
    cl = p.add(VRU, "ColorLab", 0, {0: 0.6, 1: 1.4, 2: 1.0, 3: 1.2, 4: 0.3, 5: 0.6, 8: 1.0})
    lf = p.add(VRU, "LineField", 0, {0: 0.7, 1: 0.45, 2: -0.2, 4: 0.5, 5: 0.6, 6: 1.3})                  # terrain copy
    sh = p.add(VRU, "Shapes", 0, {0: 12.0, 1: 1.0, 3: 0.3, 9: 3, 10: 0.5, 12: 0.5})   # soft circles: the wipe pattern
    pt = p.add(VRU, "Shapes", 0, {0: 12.0, 1: 1.0, 3: 0.2, 9: 6, 10: 0.7, 12: 0.02, 13: 1})  # six portholes: the key matte
    vm = p.add(VRU, "VideoMixer", 0, {13: 0, 14: 0.55, 15: 2, 16: 0.3, 18: 0.35, 19: 0.15, 22: 1})
    dp = p.add(VRU, "Display", 0)
    p.cable(pl, 0, fb, 0)
    p.cable(pl, 0, fd, 0); p.cable(fd, 0, cz, 0)
    p.cable(pl, 0, ff, 0); p.cable(ff, 0, cl, 0)
    p.cable(pl, 0, lf, 0)
    p.cable(fb, 0, vm, 0); p.cable(cz, 0, vm, 1); p.cable(cl, 0, vm, 2); p.cable(lf, 0, vm, 3)
    p.cable(sh, 0, vm, 4)                                     # circles -> wipe PATTERN
    p.cable(cz, 0, vm, 5); p.cable(pt, 0, vm, 6)              # key: the colour-band copy through six portholes
    p.cable(vm, 0, dp, 0)
    k = conductor(p, 1, 0.7, 0.07,
                  [[1.2, 3.7, 6.2, 8.7, 3.7, 1.2, 8.7, 6.2],  # row 1 -> Mixer NEXT CV (which copy is next)
                   [0, 2, 5, 1, 7, 3, 9, 4],                  # row 2 -> Colorizer OFFSET
                   [0, 3, 0, 6, 1, 8, 2, 5]],                 # row 3 -> Color Lab HUE
                  gates=[1, 0, 1, 1, 0, 0, 1, 0])             # TRIG out: the beats the key is on
    ch = chaos(p, 1, 0.4)
    vt = vert(p, 1, [0.6, 0.6, 0.5, 0.5, 0, 0, 0, 0])
    pr = probe(p, 1, vm)
    p.notes(2, "70s III GLOBAL GROOVE\n"
            "A collage in the manner of Paik: four copies of the footage that look nothing alike (a trail of receding "
            "echoes, a delayed copy in flat colour bands, a frozen solarized copy, a line-drawn terrain), cut and wiped "
            "against each other, with six portholes of the colour-band copy keyed over the top.\n"
            "Every beat the SEQ3 phrase picks which copy is next and re-tints two of them. Each downbeat an AUTO take wipes "
            "to it through a pattern of soft circles; Random strikes cut straight to it, sparse at first, a barrage near 0:40, "
            "almost none around 1:50. On four beats in eight the portholes open, and they jump to a new angle each time "
            "the chaos steps. The freeze re-grabs every 2.5 s and strobes faster as the 67 s macro rises; the delay jumps "
            "to a new length on each strike.\n"
            "The probe reads the picture under its spot and raises the key threshold when it is bright.\n"
            "Play: Video Mixer TIME, the porthole Shapes SIZE, Random PROB.")
    p.cable(k["seq"], Q_ROW + 0, vm, 11, GREEN)
    p.cable(k["seq"], Q_ROW + 1, cz, 7, GREEN)
    p.cable(k["seq"], Q_ROW + 2, cl, 1, GREEN)
    p.cable(k["seq"], Q_TRIG, vm, 16, RED)                    # gated beats -> KEY ON
    p.cable(k["seq"], Q_STEP + 0, vm, 9, RED)                 # downbeat -> AUTO take (wipe)
    p.cable(k["rnd"], R_TRIG, vm, 8, RED)                     # strike -> CUT
    p.cable(k["rnd"], R_TRIG, fb, 8, RED)                     # strike -> clear the echoes
    p.cable(k["rnd"], R_STEP, fd, 1, GREEN)                   # stepped -> Frame Delay FRAMES
    p.cable(ch, 5, ff, 2, RED)                                # counter-clock -> Freeze TRIG
    p.cable(pr, P_LUMA, vt, 0, BLUE); p.cable(vt, 0, vm, 15, BLUE)   # brightness -> key THRESH
    p.cable(k["mB"], TRI, vt, 1, YEL); p.cable(vt, 1, ff, 4, YEL)    # macro B -> Freeze STROBE
    p.cable(ch, 4, vt, 2, YEL); p.cable(vt, 2, pt, 7, YEL)           # stepped chaos -> porthole TURN
    p.cable(k["mA"], SIN, vt, 3, YEL); p.cable(vt, 3, lf, 4, YEL)    # macro A -> terrain Z SPIN
    return p


# -------------------------------------------------------------------------------------------------
# 4. The raster bent under the Deflection Yoke: snaps and slow recoveries.
def yoke_snap():
    p = Patch()
    pl = player(p, 0)
    ws = p.add(VRU, "WaveShaper", 0, {**SPHERE_WS, 7: 0.55})
    ff = p.add(VRU, "FreezeFrame", 0, {0: 0.0, 1: 1.0, 2: 0.0})
    yk = p.add(VRU, "DeflectionYoke", 0, {0: 0.12, 1: 0.08, 2: 0.5, 3: 0.514, 5: 0.15, 6: 0.0, 9: 0.547, 13: 2, 14: 240,
                                          15: 0.2, 16: 0.646, 17: 0.25, 18: 0.2, 19: 0.0, 20: 0.6, 21: 0.486, 22: 0.617})
    fo = p.add(VRU, "Focus", 0, {0: 0.08, 1: 0.0})
    cl = p.add(VRU, "ColorLab", 0, {0: 0.55, 1: 0.7, 2: 1.0, 3: 1.15, 4: 0.0, 5: 0.15, 8: 1.0})
    dp = p.add(VRU, "Display", 0)
    chain(p, [pl, ws, ff, yk, fo, cl, dp])
    k = conductor(p, 1, 0.9, 0.06,
                  [[0, 1, 0, 2, 0.5, 3, 0, 1.5],              # row 1 -> yoke H coil RATE (1 V/oct)
                   [0, 0, 5, 0, 2.5, 0, 5, 7.5],              # row 2 -> yoke LINES (5 V doubles)
                   [0, 1.5, 0, 3, 0, 0.8, 2, 0]])             # row 3 -> yoke TWIST
    e1 = hit(p, 1, 0.7, 0.82)                                 # downbeat snap, ~2 s recovery
    e2 = hit(p, 1, 0.6, 0.86)                                 # struck snap, ~3 s recovery
    ch = chaos(p, 1, 0.19, uni=False)
    vt = vert(p, 1, [0.45, 0.4, 0.5, 0.5, 0.6, 0.6, 0.5, 0])
    pr = probe(p, 1, cl)
    p.notes(2, "70s III YOKE SNAP\n"
            "The footage on a raster whose deflection coils are kicked and left to ring: Paik and Abe's wobbulator, played.\n"
            "At rest the raster is almost straight. Each downbeat an envelope snaps the horizontal coil hard and lets it "
            "recover over two seconds; Random strikes snap the vertical coil and re-phase the coils (sparse at first, "
            "crowded near 0:40, almost still around 1:50). Every beat the SEQ3 phrase sets the coil speed, the number of "
            "scan lines and the twist; on beat seven the picture flips. A chaos step every 5 s re-sets the S coil and "
            "freezes the picture for a moment. The 150 s macro also moves the tempo and the phosphor persistence; the "
            "67 s macro deepens all the coils.\n"
            "The probe reads the picture's motion and smears the focus when it moves.\n"
            "Play: Deflection Yoke DEPTH, PERSIST, 8vert knob 1 (snap size).")
    p.cable(k["seq"], Q_ROW + 0, yk, 4, GREEN)
    p.cable(k["seq"], Q_ROW + 1, yk, 22, GREEN)
    p.cable(k["seq"], Q_ROW + 2, yk, 19, GREEN)
    p.cable(k["seq"], Q_STEP + 0, e1, 4, RED)
    p.cable(k["seq"], Q_STEP + 6, yk, 27, RED)                # beat seven -> FLIP H gate
    p.cable(k["rnd"], R_TRIG, e2, 4, RED)
    p.cable(k["rnd"], R_TRIG, yk, 5, RED)                     # strike -> coil phase RESET
    p.cable(ch, 5, ff, 2, RED)                                # counter-clock -> Freeze TRIG
    p.cable(e1, 0, vt, 0, YEL); p.cable(vt, 0, yk, 11, YEL)   # snap -> H coil amount
    p.cable(e2, 0, vt, 1, YEL); p.cable(vt, 1, yk, 12, YEL)   # snap -> V coil amount
    p.cable(ch, 4, vt, 2, YEL); p.cable(vt, 2, yk, 13, YEL)   # stepped chaos -> S coil amount
    p.cable(k["mA"], SIN, vt, 3, YEL); p.cable(vt, 3, yk, 25, YEL)  # macro A -> PERSIST
    p.cable(k["mB"], TRI, vt, 4, YEL); p.cable(vt, 4, yk, 3, YEL)   # macro B -> DEPTH
    p.cable(k["rnd"], R_STEP, vt, 5, GREEN); p.cable(vt, 5, ff, 1, GREEN)  # stepped -> Freeze HOLD
    p.cable(pr, P_MOTION, vt, 6, BLUE); p.cable(vt, 6, fo, 2, BLUE)  # motion -> Focus SMEAR
    return p


# -------------------------------------------------------------------------------------------------
# 5. Iris terrain (my own): the footage as a line landscape, with an iris of colour footage that opens on the beat.
def iris_terrain():
    p = Patch()
    pl = player(p, 0)
    ff = p.add(VRU, "FreezeFrame", 0, {0: 0.3, 1: 1.0, 2: 0.0})
    lf = p.add(VRU, "LineField", 0, {0: 0.35, 1: 0.25, 2: -0.3, 3: 0.0, 4: 0.5, 5: 0.35, 6: 1.2, 7: 1.0})
    fb = p.add(VRU, "Feedback", 0, {0: 0.06, 1: 0.01, 4: 0.05, 5: 0.35, 6: 0.15, 8: 0, 9: 1})
    cl = p.add(VRU, "ColorLab", 0, {0: 0.45, 1: 1.2, 2: 1.0, 3: 1.2, 4: 0.0, 5: 0.2, 8: 1.0})
    cz = p.add(VRU, "Colorizer", 0, {0: 0.95, 1: 0.1, 2: 0.6, 3: 0.33, 4: 1, 5: 1, 6: 1, 7: 1, 8: 1.0, 9: 5, 10: 0.0})
    sh = p.add(VRU, "Shapes", 0, {0: 12.0, 1: 1.0, 3: 0.2, 12: 0.03, 13: 1})       # one round white matte
    vm = p.add(VRU, "VideoMixer", 0, {13: 1, 14: 0.3, 15: 0, 17: 1, 18: 0.5, 19: 0.1, 22: 1})
    dp = p.add(VRU, "Display", 0)
    chain(p, [pl, ff, lf, fb, cl])
    p.cable(pl, 0, cz, 0)
    p.cable(cl, 0, vm, 0); p.cable(ff, 0, vm, 1)             # A terrain, B the stuttered footage itself
    p.cable(cz, 0, vm, 5); p.cable(sh, 0, vm, 6)             # key: colour footage through the circle matte
    p.cable(vm, 0, dp, 0)
    k = conductor(p, 1, 0.75, 0.07,
                  [[0, 3, 6, 1, 8, 2, 5, 10],                 # row 1 -> Line Field DEPTH
                   [0, 0, 4, 0, 6, 0, 2, 5],                  # row 2 -> Line Field LINES
                   [0, 2.5, 5, 7.5, 1, 3.5, 6, 8.5]],         # row 3 -> Colorizer OFFSET
                  prob=0.1, prob_cv=0.7, density=False)
    e1 = hit(p, 1, 0.8, 0.86, attack=0.45)                    # the iris: opens in 0.06 s, closes over ~3 s
    e2 = hit(p, 1, 0.65, 0.72)
    ch = chaos(p, 1, 0.13, 0.03, uni=False, slew=0.6)
    vt = vert(p, 1, [0.6, 1.0, 0.5, 0.6, 0.5, 0.6, 0, 0])
    pr = probe(p, 1, vm, level=0.55)
    p.notes(2, "70s III IRIS TERRAIN\n"
            "The footage turned into a landscape of scan lines, with a round iris of colour footage that opens on the beat "
            "and wanders over it, in the spirit of the Vasulkas' Noisefields circle.\n"
            "Every beat the SEQ3 phrase sets the height of the terrain, the density of its lines and the colour of the iris. "
            "Each downbeat the iris opens wide and slowly closes. The picture conducts itself: the more it moves, the more "
            "Random strikes there are, and each strike freezes a new frame of the terrain and flares the feedback. When the "
            "probed spot turns bright the mixer cuts between the terrain and the plain stuttering footage. The 150 s macro "
            "drags the tempo and how much of the terrain is frozen; the 67 s macro turns its colour; a chaos step spins it.\n"
            "Play: Shapes SIZE, Line Field PERSPECTIVE, Video Probe LEVEL.")
    p.cable(k["seq"], Q_ROW + 0, lf, 1, GREEN)
    p.cable(k["seq"], Q_ROW + 1, lf, 5, GREEN)
    p.cable(k["seq"], Q_ROW + 2, cz, 7, GREEN)
    p.cable(k["seq"], Q_STEP + 0, e1, 4, RED)
    p.cable(k["rnd"], R_TRIG, e2, 4, RED)
    p.cable(k["rnd"], R_TRIG, ff, 2, RED)                     # strike -> Freeze TRIG
    p.cable(k["rnd"], R_STEP, ff, 4, GREEN)                   # stepped -> Freeze STROBE
    p.cable(pr, P_GATE, vm, 8, RED)                           # bright spot -> CUT
    p.cable(e1, 0, vt, 0, YEL); p.cable(vt, 0, sh, 3, YEL)    # iris -> Shapes SIZE
    p.cable(pr, P_MOTION, vt, 1, BLUE); p.cable(vt, 1, k["rnd"], 4, BLUE)   # motion -> Random PROB
    p.cable(e2, 0, vt, 2, YEL); p.cable(vt, 2, fb, 6, YEL)    # strike -> Feedback GAIN
    p.cable(k["mA"], SIN, vt, 3, YEL); p.cable(vt, 3, ff, 1, YEL)   # macro A -> Freeze HOLD
    p.cable(k["mB"], TRI, vt, 4, YEL); p.cable(vt, 4, cl, 1, YEL)   # macro B -> Color Lab HUE
    p.cable(ch, 4, vt, 5, YEL); p.cable(vt, 5, lf, 4, YEL)          # stepped chaos -> Line Field Z SPIN
    p.cable(ch, 2, sh, 4, YEL); p.cable(ch, 3, sh, 5, YEL)          # chaos X, Y -> iris position
    return p


# -------------------------------------------------------------------------------------------------
# 4b. Yoke snap, second pass: the bending acts on a legible picture (the sphere's grey relief on visible scanlines).
def yoke_snap_b():
    p = Patch()
    pl = player(p, 0)
    ws = p.add(VRU, "WaveShaper", 0, dict(SPHERE_WS))                # the sphere's grey relief, full strength
    ff = p.add(VRU, "FreezeFrame", 0, {0: 0.0, 1: 1.0, 2: 0.0})
    ro = p.add(VRU, "Rotator", 0, {2: 0.4})                          # enlarge the sphere to fill the raster
    yk = p.add(VRU, "DeflectionYoke", 0, {0: 0.0, 1: 0.0, 2: 0.6, 3: 0.514, 5: 0.0, 9: 0.547, 13: 2, 14: 170,
                                          15: 0.4, 16: 0.9, 17: 0.04, 18: 0.08, 19: 0.0, 20: 0.514, 21: 0.94,
                                          22: 0.94})          # full-size raster, coarse lines, sharp coils, no smear
    cl = p.add(VRU, "ColorLab", 0, {0: 0.031, 1: 0.2, 2: 0.7, 3: 1.35, 4: 0.0, 5: 0.0, 8: 1.0})
    dp = p.add(VRU, "Display", 0)
    chain(p, [pl, ws, ff, ro, yk, cl, dp])
    k = conductor(p, 1, 0.9, 0.06,
                  [[0, 1, 0, 2, 0.5, 3, 0, 1.5],              # row 1 -> yoke H coil RATE (1 V/oct)
                   [0, -2, 3, 0, -3, 0, 5, -1],               # row 2 -> yoke LINES (5 V doubles, -5 V halves)
                   [0, 1.5, 0, 3, 0, 0.8, 2, 0]])             # row 3 -> yoke TWIST
    e1 = hit(p, 1, 0.7, 0.82)                                 # downbeat snap, ~2 s recovery
    e2 = hit(p, 1, 0.6, 0.86)                                 # struck snap, ~3 s recovery
    ch = chaos(p, 1, 0.19, uni=False)
    vt = vert(p, 1, [0.5, 0.45, 0.5, 0.6, 0.5, 0.5, 0.3, 0])
    pr = probe(p, 1, cl)
    p.notes(2, "70s III YOKE SNAP B\n"
            "The sphere's grey relief drawn on a coarse raster you can see line by line, whose deflection coils are kicked "
            "and left to ring: Paik and Abe's wobbulator, played.\n"
            "At rest the raster is straight and the picture plain. Each downbeat an envelope snaps the horizontal coil "
            "hard and it rings back over two seconds; Random strikes snap the vertical coil and re-phase the coils "
            "(sparse at first, crowded near 0:40, almost still around 1:50). Every beat the SEQ3 phrase sets the coil "
            "speed, the number of scanlines (coarse to fine) and the twist; beat seven flips the picture. The freeze "
            "grabs a frame on the chaos clock and a strike sometimes holds it, so the bending plays on a still.\n"
            "The 150 s macro turns the tint round the colour wheel and the 67 s macro moves it from grey to full colour, "
            "so the sections change tone. The probe reads movement and bows the lines when the picture is busy.\n"
            "Play: Deflection Yoke DEPTH, LINES, 8vert knob 1 (snap size).")
    p.cable(k["seq"], Q_ROW + 0, yk, 4, GREEN)
    p.cable(k["seq"], Q_ROW + 1, yk, 22, GREEN)
    p.cable(k["seq"], Q_ROW + 2, yk, 19, GREEN)
    p.cable(k["seq"], Q_STEP + 0, e1, 4, RED)
    p.cable(k["seq"], Q_STEP + 6, yk, 27, RED)                # beat seven -> FLIP H gate
    p.cable(k["rnd"], R_TRIG, e2, 4, RED)
    p.cable(k["rnd"], R_TRIG, yk, 5, RED)                     # strike -> coil phase RESET
    p.cable(ch, 5, ff, 2, RED)                                # counter-clock -> Freeze TRIG
    p.cable(e1, 0, vt, 0, YEL); p.cable(vt, 0, yk, 11, YEL)   # snap -> H coil amount
    p.cable(e2, 0, vt, 1, YEL); p.cable(vt, 1, yk, 12, YEL)   # snap -> V coil amount
    p.cable(ch, 4, vt, 2, YEL); p.cable(vt, 2, yk, 13, YEL)   # stepped chaos -> S coil amount
    p.cable(k["mA"], SIN, vt, 3, YEL); p.cable(vt, 3, cl, 1, YEL)   # macro A -> Color Lab HUE
    p.cable(k["mB"], TRI, vt, 4, YEL); p.cable(vt, 4, cl, 2, YEL)   # macro B -> Color Lab SAT (grey to colour)
    p.cable(k["rnd"], R_STEP, vt, 5, GREEN); p.cable(vt, 5, ff, 1, GREEN)  # stepped -> Freeze HOLD
    p.cable(pr, P_MOTION, vt, 6, BLUE); p.cable(vt, 6, yk, 18, BLUE)  # motion -> line CURL
    return p


# -------------------------------------------------------------------------------------------------
# 2b. Feedback architecture, second pass: the footage is the feedback source; a polygon matte keys the architecture
# over the plain picture, and the 150 s macro swings the whole piece between picture and architecture.
def feedback_architecture_b():
    p = Patch()
    pl = player(p, 0)
    ws = p.add(VRU, "WaveShaper", 0, dict(SPHERE_WS))                # the recognisable picture: the sphere's relief
    fb = p.add(VRU, "Feedback", 0, {0: 0.15, 1: 0.0, 4: 0.04, 5: 0.6, 6: 0.3, 8: 0, 9: 1})   # footage tunnel
    ro = p.add(VRU, "Rotator", 0, {2: -0.1, 7: 3, 8: 3, 9: 1, 10: 0.3})          # three folds only, mirror edge
    sh = p.add(VRU, "Shapes", 0, {0: 5.0, 1: 0.15, 3: 0.2, 12: 0.03, 13: 1})     # polygon matte
    vm = p.add(VRU, "VideoMixer", 0, {13: 0, 14: 0.3, 15: 0, 18: 0.5, 19: 0.05, 22: 1})  # A picture, B architecture
    fd = p.add(VRU, "FrameDelay", 0, {0: 3.0, 1: 1.0})
    cl = p.add(VRU, "ColorLab", 0, {0: 0.031, 1: 0.35, 2: 1.0, 3: 1.1, 4: 0.0, 5: 0.0, 8: 1.0})
    dp = p.add(VRU, "Display", 0)
    p.cable(pl, 0, ws, 0); chain(p, [ws, fb, ro])
    p.cable(ws, 0, vm, 0); p.cable(ro, 0, vm, 1)              # A the picture, B the folded architecture
    p.cable(ro, 0, vm, 5); p.cable(sh, 0, vm, 6)              # key: the folded tunnel through the polygon
    chain(p, [vm, fd, cl, dp])
    k = conductor(p, 1, 0.6, 0.08,
                  [[0, 1, 2, 0.5, 1.5, 0, 2.5, 1],            # row 1 -> Feedback ZOOM (tunnel in or out)
                   [0, 0.5, 1, 0.2, 0.8, 0, 1.2, 0.3],        # row 2 -> Feedback ROTATE (spiral)
                   [0, 1, 3, 5, 0.5, 4, 2, 8]],               # row 3 -> Shapes SIDES
                  gates=[1, 1, 0, 1, 0, 1, 1, 0])             # TRIG out: the beats the polygon key is open
    saw = lfo(p, 1, 5.0)                                      # the build: a ramp restarted every downbeat
    e1 = hit(p, 1, 0.7, 0.72)
    ch = chaos(p, 1, 0.11, 0.023, uni=False)
    vt = vert(p, 1, [0.25, 0.6, 1.0, 1.0, 0.5, 0, 0, 0])
    pr = probe(p, 1, cl)
    p.notes(2, "70s III FEEDBACK ARCHITECTURE B\n"
            "The footage's grey relief is the only source: it feeds a Feedback tunnel and a three-fold Rotator. The Video "
            "Mixer swings slowly between the plain picture and that folded architecture, and on the phrase's beats keys "
            "the folded tunnel through a polygon over whichever is showing.\n"
            "Each bar of eight beats the tunnel's gain ramps up and it builds; the downbeat clears it. Every beat the SEQ3 "
            "phrase sets the tunnel's zoom and spiral and the polygon's sides; mid-bar an envelope throws the polygon "
            "open. Random strikes notch the folds round and re-time the echo. The 150 s macro moves the tempo, the "
            "density and the colour; the 67 s macro moves the mixer lever, so the piece swings from picture to "
            "architecture and back about every half minute.\n"
            "The probe reads the brightness: when the picture whites out, the feedback key rises and thins it.\n"
            "Play: Video Mixer FADER, Feedback GAIN, Rotator FOLDS.")
    p.cable(k["seq"], Q_ROW + 0, fb, 1, GREEN)
    p.cable(k["seq"], Q_ROW + 1, fb, 2, GREEN)
    p.cable(k["seq"], Q_ROW + 2, sh, 0, GREEN)
    p.cable(k["seq"], Q_TRIG, vm, 16, RED)                    # gated beats -> KEY ON
    p.cable(k["seq"], Q_STEP + 0, fb, 8, RED)                 # downbeat -> Feedback CLEAR
    p.cable(k["seq"], Q_STEP + 0, saw, 2, RED)                # downbeat -> ramp RESET
    p.cable(k["seq"], Q_STEP + 4, e1, 4, RED)                 # mid-bar -> polygon envelope
    p.cable(k["rnd"], R_TRIG, ro, 6, RED)                     # strike -> Rotator NOTCH
    p.cable(k["rnd"], R_STEP, fd, 1, GREEN)                   # stepped -> Frame Delay FRAMES
    p.cable(saw, SAW, vt, 0, YEL); p.cable(vt, 0, fb, 6, YEL)       # ramp -> Feedback GAIN
    p.cable(e1, 0, vt, 1, YEL); p.cable(vt, 1, sh, 3, YEL)          # envelope -> Shapes SIZE
    p.cable(k["mB"], TRI, vt, 2, YEL); p.cable(vt, 2, vm, 7, YEL)   # macro B -> Mixer FADER (picture <-> architecture)
    p.cable(k["mA"], SIN, vt, 3, YEL); p.cable(vt, 3, cl, 1, YEL)   # macro A -> Color Lab HUE
    p.cable(ch, 0, sh, 7, YEL)                                      # chaos A -> Shapes TURN
    p.cable(ch, 1, ro, 1, YEL)                                      # chaos B -> Rotator ANGLE
    p.cable(pr, P_AVG, vt, 4, BLUE); p.cable(vt, 4, fb, 7, BLUE)    # brightness -> Feedback KEY
    return p


PATCHES = [("70s III struck sphere.vcv", struck_sphere), ("70s III feedback architecture.vcv", feedback_architecture),
           ("70s III global groove.vcv", global_groove), ("70s III yoke snap.vcv", yoke_snap),
           ("70s III iris terrain.vcv", iris_terrain),
           ("70s III yoke snap b.vcv", yoke_snap_b), ("70s III feedback architecture b.vcv", feedback_architecture_b)]

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    only = sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    for name, fn in PATCHES:
        if only and not any(o in name for o in only):
            continue
        print(Patch.save(fn(), out, name))
