#!/usr/bin/env python3
"""Write five self-playing studio patches ("70s IV ..."), built the way a generative audio patch is built.

Each patch has three sections with jobs, as in a Saturata-style modular piece:
  - timing: every picture layer is a voice with its own Marbles running its own clock, at rates in the ratio
    3 : 4 : 7, so the layers drift in and out of step (they line up again only every ten seconds or so);
  - voices: each Marbles keeps a short looping pattern (deja vu) that slowly mutates, and plays its own layer;
  - mix and output: three patches are built round the Video Mixer, whose editing (hard cuts, timed takes,
    pattern wipes, T-bar moves, the key layer) is chosen by an editor Marbles of its own; switching has
    consequences, because each channel's LIVE output acts on that channel's own treatment.
A slow macro LFO (about three and a half minutes) locks and unlocks every loop, so the piece has sections.
Only VRU, Fundamental and Audible Instruments (Marbles) modules, plus a Notes panel.

Ids checked against source: AudibleInstruments v2 (2.0.0) src/Marbles.cpp; Fundamental v2.6.4 LFO, ADSR, 8vert,
Random, Rescale; VRU origin/main VideoMixer.cpp and video_mixer_math.hpp.

Usage: scripts/make-70s-iv-patches.py [output-dir] [name ...]"""
import importlib.util, math, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(fname, modname):
    spec = importlib.util.spec_from_file_location(modname, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


m70 = _load("make-70s-patches.py", "m70")
m3 = _load("make-70s-iii-patches.py", "m3")
Patch, VRU, FUND = m70.Patch, m70.VRU, m70.FUND
RED, YEL, GREEN, BLUE, PURPLE = m70.RED, m70.YEL, m70.GREEN, m70.BLUE, m70.PURPLE
lfo_freq, OUT = m70.lfo_freq, m70.OUT
AI = "AudibleInstruments"
m70.VER[AI] = "2.0.0"
_panel_hp = m70.panel_hp


def _hp(plugin, model):
    if plugin == AI:
        svg = os.path.join(os.path.expanduser("~/Library/Application Support/Rack2/plugins-mac-arm64"), AI, "res",
                           model + ".svg")
        try:
            m = re.search(r'\swidth="([\d.]+)(mm|px)?"', open(svg, errors="ignore").read(4000))
            w = float(m.group(1))
            return int(round(w / 5.08 if m.group(2) == "mm" else w / 15.0))
        except (OSError, AttributeError):
            return 18                                        # Marbles is 18 HP
    return _panel_hp(plugin, model)


m70.panel_hp = _hp

player, chain, lfo, vert, probe = m3.player, m3.chain, m3.lfo, m3.vert, m3.probe
SPHERE_WS, SPHERE_FB, SPHERE_SP, SPHERE_CL = m3.SPHERE_WS, m3.SPHERE_FB, m3.SPHERE_SP, m3.SPHERE_CL
SIN, TRI, SAW, SQR = 0, 1, 2, 3
P_LUMA, P_AVG, P_MOTION, P_GATE = 0, 4, 5, 6

# Marbles (AudibleInstruments 2.0.0): outputs and inputs
T1, T2, T3, Y, X1, X2, X3 = range(7)
I_TBIAS, I_XBIAS, I_TCLK, I_TRATE, I_JIT, I_DV, I_STEPS, I_SPREAD, I_XCLK = range(9)
# Video Mixer inputs and outputs
VM_PIC, VM_PATTERN, VM_KEY, VM_MATTE = 0, 4, 5, 6
VM_FADER, VM_CUT, VM_AUTO, VM_NEXT, VM_NEXTCV = 7, 8, 9, 10, 11
VM_THRESH, VM_KEYON = 15, 16
VM_LIVE = 1                                                   # LIVE A..D are outputs 1..4

LOOP = [1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 6, 6, 6, 7, 7, 8, 8, 8, 10, 10, 12, 12, 12, 14, 14,
        16, 16]
UNIT = 0.1                                                    # polymetre unit: voices at 3, 4 and 7 units (Hz)


def marbles(p, row, hz, steps_len=8, dv=0.42, bias=0.5, spread=0.5, xrange=2, trange=0, ydiv=8):
    """A Marbles on its own internal clock at hz. Deja vu on for t and x; DEJA_VU 0.5 is a locked loop,
    lower mutates (p = (2d - 1)^2 per step). steps_len is the loop length in steps."""
    base = {0: 0.5, 1: 2.0, 2: 8.0}[trange]
    rate = math.log2(hz / base) / 5.0
    idx = [i for i, n in enumerate(LOOP) if n == steps_len]
    length = idx[len(idx) // 2] / 35.0
    return p.add(AI, "Marbles", row,
                 {2: dv, 3: rate, 4: spread, 7: length, 8: bias, 9: 0.5, 13: 0.0, 14: 0.5},
                 data={"t_deja_vu": True, "x_deja_vu": True, "t_mode": 0, "x_mode": 0, "t_range": trange,
                       "x_range": xrange, "external": False, "x_scale": 0, "y_divider_index": ydiv,
                       "x_clock_source_internal": 0})


def env(p, row, a, d, s, r):
    """Fundamental ADSR; time constant = 1 ms x 10000^knob."""
    return p.add(FUND, "ADSR", row, {0: a, 1: d, 2: s, 3: r})


def rescale(p, row, gain, offset, lo, hi):
    return p.add(FUND, "Rescale", row, {0: gain, 1: offset, 2: hi, 3: lo}, data={"multiplier": 1.0})


def take_time(seconds):
    """Video Mixer TIME knob for a take of this many seconds (0.02 x 1000^v)."""
    return math.log(seconds / 0.02) / math.log(1000.0)


def mixer(p, row, extra):
    """Video Mixer with every COLOR off, so each picture keeps the footage's own colour."""
    return p.add(VRU, "VideoMixer", row, {4: 0, 5: 0, 6: 0, 7: 0, **extra})


def macro_to(p, k, lfo_id, out, vt, ch, dest, inp, gain_note=YEL):
    p.cable(lfo_id, out, vt, ch, gain_note)
    p.cable(vt, ch, dest, inp, gain_note)


# -------------------------------------------------------------------------------------------------
# 1. Four voices: four treatments of the footage, each its own voice, cut and wiped by an editor.
def four_voices():
    p = Patch()
    pl = player(p, 0)
    wsA = p.add(VRU, "WaveShaper", 0, dict(SPHERE_WS))
    fbA = p.add(VRU, "Feedback", 0, dict(SPHERE_FB))
    roA = p.add(VRU, "Rotator", 0, {2: 0.4, 9: 2, 10: 0.8})                      # notch 1/8 turn, glide
    lfB = p.add(VRU, "LineField", 0, {0: 0.45, 1: 0.3, 2: -0.3, 4: 0.5, 5: 0.4, 6: 1.25, 7: 1.0})
    roB = p.add(VRU, "Rotator", 0, {2: 0.45, 7: 3})
    fdC = p.add(VRU, "FrameDelay", 0, {0: 10.0, 1: 1.0})
    fbC = p.add(VRU, "Feedback", 0, {0: -0.3, 1: 0.03, 2: 0.1, 3: -0.04, 4: 0.0, 5: 0.9, 6: 0.0, 8: 2, 9: 0})
    roC = p.add(VRU, "Rotator", 0, {2: 0.4, 7: 3})
    ffD = p.add(VRU, "FreezeFrame", 0, {0: 1.0, 1: 1.0, 2: 0.0})
    foD = p.add(VRU, "Focus", 0, {0: 0.1, 1: 0.45})
    roD = p.add(VRU, "Rotator", 0, {2: 0.4, 7: 3, 8: 4})                         # four-fold mirror
    sh = p.add(VRU, "Shapes", 0, {0: 12.0, 1: 1.0, 3: 0.4, 9: 4, 10: 0.45, 11: 0.3, 12: 0.5})  # wipe pattern
    vm = mixer(p, 0, {13: 0, 14: take_time(1.6), 15: 2, 16: 0.15, 17: 0, 18: 0.3, 19: 0.1, 22: 1})
    dp = p.add(VRU, "Display", 0)
    chain(p, [pl, wsA, fbA, roA])
    chain(p, [pl, lfB, roB])
    chain(p, [pl, fdC, fbC, roC])
    chain(p, [pl, ffD, foD, roD])
    for i, src in enumerate([roA, roB, roC, roD]):
        p.cable(src, 0, vm, VM_PIC + i)
    p.cable(sh, 0, vm, VM_PATTERN)
    p.cable(lfB, 0, vm, VM_KEY)                                                   # key layer: the line landscape
    p.cable(vm, 0, dp, 0)

    ed = marbles(p, 1, 0.25, steps_len=8, dv=0.36, bias=0.4, spread=0.5)                              # the editor
    vA = marbles(p, 1, 3 * UNIT, steps_len=5)
    vB = marbles(p, 1, 4 * UNIT, steps_len=7)
    vC = marbles(p, 1, 7 * UNIT, steps_len=8)
    rs = rescale(p, 1, 1.0, 6.0, 1.0, 10.0)                                       # +-5 V -> 1..10 V: A the rarest band
    mA = lfo(p, 1, 210.0)
    mB = lfo(p, 1, 83.0)
    eB = env(p, 1, 0.72, 0.5, 1.0, 0.8)                                           # B rises while on air
    vt = vert(p, 1, [0.12, 0.1, 0.1, 0.1, 0.15, 0.5, 0.12, 0.3])
    vv = vert(p, 1, [0.15, 0.4, 0.6, 0.4, 0.2, 0.1, 0.5, 1.0])
    pr = probe(p, 1, vm)
    p.notes(2, "70s IV FOUR VOICES\n"
            "Four treatments of the footage are four voices: A the sphere (contours, echo, turned), B a line landscape, "
            "C receding echoes, D a frozen moment folded four ways. A, B and C each have their own Marbles, running at "
            "3, 4 and 7 beats to every ten seconds, so they fall in and out of step; each loops a short pattern that "
            "slowly changes.\n"
            "An editor Marbles picks the next picture (its X voltage, in four bands) and on each beat either cuts or "
            "takes it with a wipe whose shape changes on every take (soft blooms to hard rings).\n"
            "Switching has consequences: A turns a notch whenever it comes on; B's landscape rises while it is on air; "
            "C's echoes restart from the moment it comes on; D freezes the instant it is chosen. While C is on, the "
            "line landscape is keyed over it, its bright lines only.\n"
            "The 210 s macro locks and loosens every loop and speeds the editor; the 83 s macro tilts it from cuts to "
            "wipes and grows the wipe pattern.\n"
            "Play: editor Marbles BIAS (cuts or wipes), DEJA VU on any voice, Video Mixer TIME.")
    # editor
    p.cable(ed, T1, vm, VM_CUT, RED)
    p.cable(ed, T3, vm, VM_AUTO, RED)
    p.cable(ed, X2, rs, 0, GREEN); p.cable(rs, 0, vm, VM_NEXTCV, GREEN)
    p.cable(ed, X3, sh, 9, GREEN)                                                 # wipe edge spread, per take
    p.cable(ed, X1, sh, 7, GREEN)                                                 # wipe pattern turn
    # macros
    macro_to(p, None, mA, SIN, vt, 0, ed, I_TRATE)
    p.cable(vt, 1, vA, I_DV, YEL); p.cable(vt, 2, vB, I_DV, YEL); p.cable(vt, 3, vC, I_DV, YEL)
    macro_to(p, None, mB, TRI, vt, 4, ed, I_TBIAS)
    p.cable(vt, 5, sh, 3, YEL)                                                    # macro B -> pattern SIZE
    p.cable(vt, 6, ed, I_DV, YEL)                                                 #   -> editor's loop, locked to loose
    p.cable(pr, P_AVG, vt, 7, BLUE); p.cable(vt, 7, vm, VM_THRESH, BLUE)          # brightness -> key THRESH
    # voices
    p.cable(vA, X1, vv, 0, GREEN); p.cable(vv, 0, fbA, 5, GREEN)                  # A: echo hue
    p.cable(vA, X2, vv, 1, GREEN); p.cable(vv, 1, wsA, 3, GREEN)                  # A: contour count
    p.cable(vB, X1, vv, 2, GREEN); p.cable(vv, 2, lfB, 5, GREEN)                  # B: line density
    p.cable(vB, X2, vv, 3, GREEN); p.cable(vv, 3, lfB, 1, GREEN)                  # B: depth
    p.cable(vC, X1, vv, 4, GREEN); p.cable(vv, 4, fbC, 1, GREEN)                  # C: echo zoom
    p.cable(vC, X3, vv, 5, GREEN); p.cable(vv, 5, fbC, 2, GREEN)                  # C: echo rotate
    p.cable(vC, X2, vv, 7, GREEN); p.cable(vv, 7, fdC, 1, GREEN)                  # C: delay length
    # consequences of switching
    p.cable(vm, VM_LIVE + 0, roA, 6, RED)                                         # A live -> Rotator NOTCH
    p.cable(vm, VM_LIVE + 1, eB, 4, RED)                                          # B live -> envelope gate
    p.cable(eB, 0, vv, 6, YEL); p.cable(vv, 6, lfB, 2, YEL)                       #   -> landscape X TILT
    p.cable(vm, VM_LIVE + 2, fbC, 8, RED)                                         # C live -> echoes CLEAR
    p.cable(vm, VM_LIVE + 2, vm, VM_KEYON, RED)                                   # C live -> key layer on
    p.cable(vm, VM_LIVE + 3, ffD, 2, RED)                                         # D live -> Freeze TRIG
    return p


# -------------------------------------------------------------------------------------------------
# 2. Wipe round: ADVANCE walks the four pictures round; wipes, T-bar sweeps, rare cuts, key flashes.
def wipe_round():
    p = Patch()
    pl = player(p, 0)
    clA = p.add(VRU, "ColorLab", 0, {0: 0.0, 1: 1.15, 2: 1.0, 3: 1.3, 4: 0.0, 5: 0.0, 8: 1.0})
    roA = p.add(VRU, "Rotator", 0, {2: 0.4, 9: 3, 10: 0.6})                      # notch 1/6 turn
    wsB = p.add(VRU, "WaveShaper", 0, dict(SPHERE_WS))
    spB = p.add(VRU, "ScanProcessor", 0, dict(SPHERE_SP))
    fbC = p.add(VRU, "Feedback", 0, {0: 0.2, 1: 0.05, 4: 0.08, 5: 0.75, 6: 0.25, 8: 0, 9: 1})
    roC = p.add(VRU, "Rotator", 0, {2: 0.4, 7: 3, 8: 3})
    fdD = p.add(VRU, "FrameDelay", 0, {0: 20.0, 1: 1.0})
    clD = p.add(VRU, "ColorLab", 0, {0: 0.35, 1: 1.3, 2: 1.0, 3: 1.3, 4: 0.3, 5: 0.5, 8: 1.0})
    ffD = p.add(VRU, "FreezeFrame", 0, {0: 0.0, 1: 1.0, 2: 0.0})
    roB = p.add(VRU, "Rotator", 0, {2: 0.4, 7: 3})
    roD = p.add(VRU, "Rotator", 0, {2: 0.4, 7: 3, 8: 2})
    sh = p.add(VRU, "Shapes", 0, {0: 6.0, 1: 0.3, 3: 0.45, 11: 0.2, 12: 0.3})    # polygon wipe pattern
    vm = mixer(p, 0, {13: 1, 14: take_time(2.5), 15: 2, 16: 0.1, 17: 0, 18: 0.55, 19: 0.08, 22: 1})
    dp = p.add(VRU, "Display", 0)
    chain(p, [pl, clA, roA])
    p.cable(pl, 0, wsB, 0); p.cable(wsB, 0, spB, 0); p.cable(spB, 1, roB, 0)
    p.cable(pl, 0, fbC, 0); p.cable(fbC, 0, roC, 0)
    chain(p, [pl, fdD, clD, ffD, roD])
    p.cable(roA, 0, vm, 0); p.cable(roB, 0, vm, 1); p.cable(roC, 0, vm, 2); p.cable(roD, 0, vm, 3)
    p.cable(sh, 0, vm, VM_PATTERN)
    p.cable(fbC, 0, vm, VM_KEY)                                                   # key layer: the tunnel
    p.cable(vm, 0, dp, 0)

    ed = marbles(p, 1, 3 * UNIT * 0.6, steps_len=7, bias=0.35)                   # editor, 3 units (slowed)
    vV = marbles(p, 1, 4 * UNIT * 0.6, steps_len=5)                              # picture voice, 4 units
    vR = marbles(p, 1, 7 * UNIT * 0.6, steps_len=8, bias=0.8)                    # rhythm voice, 7 units
    tb = lfo(p, 1, 37.0)                                                          # the T-bar hand
    mA = lfo(p, 1, 200.0)
    mB = lfo(p, 1, 71.0)
    e1 = env(p, 1, 0.0, 0.62, 0.0, 0.62)                                          # struck hit
    vt = vert(p, 1, [0.08, 0.1, 0.1, 0.12, 0.15, 0.4, 0.6, 1.0])
    vv = vert(p, 1, [0.12, 0.25, 1.0, 0.3, 0.4, 0.5, 0.03, 0])
    pr = probe(p, 1, vm)
    p.notes(2, "70s IV WIPE ROUND\n"
            "Four pictures go round in order (ADVANCE): A the footage itself, turned; B the sphere relief; C a "
            "three-fold tunnel; D a delayed, posterised mirror copy.\n"
            "Three Marbles at 3, 4 and 7: the editor takes the next picture with a polygon wipe on its beats and on "
            "others skips one ahead, so the order is not fixed; its loop also reshapes the wipe (sides, size, melt) "
            "every beat. A slow hand moves the T-bar across about every 18 s, so long wipes cross the quick ones; "
            "rare cuts from the rhythm voice break in. The rhythm voice also strikes: the tunnel flashes through as "
            "a key and the echoes kick.\n"
            "Switching has consequences: A turns a notch as it comes on; B's relief climbs while it is live; C's tunnel "
            "starts fresh; D freezes as it comes on.\n"
            "Macros at 200 s and 71 s loosen and lock the loops, change the pace, and lean the editor between "
            "wipes and skips.\n"
            "Play: Video Mixer FADER and TIME, editor Marbles BIAS, Shapes SIDES.")
    # editor
    p.cable(ed, T1, vm, VM_AUTO, RED)
    p.cable(ed, T3, vm, VM_NEXT, RED)
    p.cable(ed, X1, sh, 3, GREEN)                                                 # pattern SIZE
    p.cable(ed, X2, sh, 0, GREEN)                                                 # pattern SIDES
    p.cable(ed, X3, sh, 8, GREEN)                                                 # pattern MELT
    # rhythm voice
    p.cable(vR, T3, vm, VM_CUT, RED)
    p.cable(vR, T1, e1, 4, RED)
    p.cable(e1, 0, vm, VM_KEYON, RED)                                             # hit -> key flash
    p.cable(e1, 0, vv, 6, YEL); p.cable(vv, 6, fbC, 6, YEL)                       # hit -> tunnel GAIN
    # the T-bar hand
    p.cable(tb, TRI, vm, VM_FADER, YEL)
    # macros
    macro_to(p, None, mA, SIN, vt, 0, ed, I_TRATE)
    p.cable(vt, 1, vV, I_DV, YEL); p.cable(vt, 2, vR, I_DV, YEL); p.cable(vt, 3, ed, I_DV, YEL)
    macro_to(p, None, mB, TRI, vt, 4, ed, I_TBIAS)
    p.cable(vt, 5, sh, 9, YEL)                                                    # macro B -> pattern SPREAD
    p.cable(pr, P_LUMA, vt, 7, BLUE); p.cable(vt, 7, vm, VM_THRESH, BLUE)
    # picture voice
    p.cable(vV, X1, vv, 0, GREEN); p.cable(vv, 0, spB, 1, GREEN)                  # relief height
    p.cable(vV, X2, vv, 1, GREEN); p.cable(vv, 1, fbC, 1, GREEN)                  # tunnel zoom
    p.cable(vV, X3, vv, 2, GREEN); p.cable(vv, 2, fdD, 1, GREEN)                  # delay length
    p.cable(vV, Y, vv, 3, GREEN); p.cable(vv, 3, roA, 1, GREEN)                   # A angle drift
    # consequences
    p.cable(vm, VM_LIVE + 0, roA, 6, RED)
    p.cable(vm, VM_LIVE + 1, vv, 4, RED); p.cable(vv, 4, wsB, 3, RED)             # B live -> more contours
    p.cable(vm, VM_LIVE + 2, fbC, 8, RED)
    p.cable(vm, VM_LIVE + 3, ffD, 2, RED)
    return p


# -------------------------------------------------------------------------------------------------
# 3. Key ladder: three pictures dissolve under a kaleidoscope key whose threshold climbs and falls.
def key_ladder():
    p = Patch()
    pl = player(p, 0)
    wsA = p.add(VRU, "WaveShaper", 0, dict(SPHERE_WS))
    roA = p.add(VRU, "Rotator", 0, {2: 0.4})
    lfB = p.add(VRU, "LineField", 0, {0: 0.5, 1: 0.25, 2: -0.25, 4: 0.45, 5: 0.5, 6: 1.3, 7: 1.0})
    fbB = p.add(VRU, "Feedback", 0, {0: 0.05, 1: 0.0, 4: 0.03, 5: 0.4, 6: 0.15, 8: 0, 9: 1})
    roB = p.add(VRU, "Rotator", 0, {2: 0.4, 7: 3})
    fdC = p.add(VRU, "FrameDelay", 0, {0: 6.0, 1: 1.0})
    clC = p.add(VRU, "ColorLab", 0, {0: 0.0, 1: 1.2, 2: 1.0, 3: 1.25, 4: 0.12, 5: 0.35, 8: 1.0})
    roC = p.add(VRU, "Rotator", 0, {2: 0.4, 7: 2})
    ffK = p.add(VRU, "FreezeFrame", 0, {0: 0.0, 1: 1.0, 2: 0.0})
    roK = p.add(VRU, "Rotator", 0, {2: 0.35, 7: 3, 8: 4, 9: 1, 10: 1.0})        # kaleidoscope key picture
    vm = mixer(p, 0, {13: 0, 14: take_time(4.0), 15: 0, 17: 0, 18: 0.5, 19: 0.06, 22: 1})
    dp = p.add(VRU, "Display", 0)
    chain(p, [pl, wsA, roA])
    chain(p, [pl, lfB, fbB, roB])
    chain(p, [pl, fdC, clC, roC])
    chain(p, [pl, ffK, roK])
    p.cable(roA, 0, vm, 0); p.cable(roB, 0, vm, 1); p.cable(roC, 0, vm, 2)
    p.cable(roK, 0, vm, VM_KEY)
    p.cable(vm, 0, dp, 0)

    ed = marbles(p, 1, 3 * UNIT * 0.5, steps_len=6)                              # editor
    vB = marbles(p, 1, 4 * UNIT * 0.5, steps_len=7)                              # landscape voice
    vK = marbles(p, 1, 7 * UNIT * 0.5, steps_len=5, bias=0.45)                   # key voice
    rs = rescale(p, 1, 0.75, 3.75, 0.0, 7.4)                                      # +-5 V -> A..C bands only
    eK = env(p, 1, 0.55, 0.78, 0.0, 0.78)                                          # key gate: on for ~2 to 4 s
    mA = lfo(p, 1, 205.0)
    mB = lfo(p, 1, 89.0)
    vt = vert(p, 1, [0.1, 0.1, 0.1, 0.1, 0.12, 0.6, 0.5, 0])
    vv = vert(p, 1, [0.5, 0.4, 0.15, 0.6, 1.0, 0.5, 0.3, 0])
    pr = probe(p, 1, vm)
    p.notes(2, "70s IV KEY LADDER\n"
            "Three pictures dissolve slowly into one another: A the sphere relief, B a line landscape with a "
            "light echo, C a delayed, solarised copy. Over them a key layer: the footage folded four ways, cut by its "
            "own brightness.\n"
            "Three Marbles at 3, 4 and 7. The editor picks the next picture (three bands of its X) and takes it with "
            "a four-second dissolve, or cuts. The key voice opens the key on its beats, and its looping X sets the "
            "threshold, so each time a different rung of brightness comes through: just the highlights, then the "
            "mid-tones, then almost all of it. The landscape voice moves its depth and density.\n"
            "Switching has consequences: when A comes on the kaleidoscope turns a notch; B's echo restarts; while C is "
            "on, the key picture freezes.\n"
            "The 205 s macro loosens and locks every loop; the 89 s macro slows and quickens the key voice.\n"
            "Play: Video Mixer THRESH and KEY SOFT, key Marbles DEJA VU, Rotator FOLDS on the key.")
    # editor
    p.cable(ed, T1, vm, VM_AUTO, RED)
    p.cable(ed, T3, vm, VM_CUT, RED)
    p.cable(ed, X2, rs, 0, GREEN); p.cable(rs, 0, vm, VM_NEXTCV, GREEN)
    # key voice
    p.cable(vK, T2, eK, 4, RED)
    p.cable(eK, 0, vm, VM_KEYON, RED)
    p.cable(vK, X2, vv, 4, GREEN); p.cable(vv, 4, vm, VM_THRESH, GREEN)            # threshold ladder
    p.cable(vK, X1, vv, 5, GREEN); p.cable(vv, 5, roK, 1, GREEN)                  # kaleidoscope angle
    p.cable(vK, T3, ffK, 2, RED)                                                  # re-capture the key picture
    # landscape voice
    p.cable(vB, X1, vv, 0, GREEN); p.cable(vv, 0, lfB, 1, GREEN)
    p.cable(vB, X2, vv, 1, GREEN); p.cable(vv, 1, lfB, 5, GREEN)
    p.cable(vB, X3, vv, 2, GREEN); p.cable(vv, 2, fbB, 5, GREEN)
    p.cable(ed, Y, vv, 3, GREEN); p.cable(vv, 3, clC, 1, GREEN)                   # C's hue, slowly
    # macros
    p.cable(mA, SIN, vt, 0, YEL)
    p.cable(vt, 0, ed, I_DV, YEL); p.cable(vt, 1, vB, I_DV, YEL); p.cable(vt, 2, vK, I_DV, YEL)
    p.cable(vt, 3, ed, I_TRATE, YEL)
    macro_to(p, None, mB, TRI, vt, 4, vK, I_TRATE)
    p.cable(pr, P_MOTION, vt, 6, BLUE); p.cable(vt, 6, fbB, 7, BLUE)              # motion -> echo KEY
    # consequences
    p.cable(vm, VM_LIVE + 0, roK, 6, RED)
    p.cable(vm, VM_LIVE + 1, fbB, 8, RED)
    p.cable(vm, VM_LIVE + 2, ffK, 1, RED)                                         # C live -> key HOLD
    return p


# -------------------------------------------------------------------------------------------------
# 4. Three coils: the colour footage on a Deflection Yoke whose three coils are three polymetric voices.
def three_coils():
    p = Patch()
    pl = player(p, 0)
    cl = p.add(VRU, "ColorLab", 0, {0: 0.0, 1: 1.25, 2: 0.9, 3: 1.15, 4: 0.0, 5: 0.0, 8: 1.0})
    ro = p.add(VRU, "Rotator", 0, {2: 0.4})
    yk = p.add(VRU, "DeflectionYoke", 0, {0: 0.0, 1: 0.0, 2: 0.6, 3: 0.514, 5: 0.0, 9: 0.547, 13: 2, 14: 200,
                                          15: 0.4, 16: 0.9, 17: 0.04, 18: 0.08, 19: 0.0, 20: 0.47, 21: 0.94,
                                          22: 0.94, 24: 1, 25: 1})
    fo = p.add(VRU, "Focus", 0, {0: 0.05, 1: 0.0})
    dp = p.add(VRU, "Display", 0)
    chain(p, [pl, cl, ro, yk, fo, dp])

    vH = marbles(p, 1, 3 * UNIT, steps_len=5, bias=0.4)
    vV = marbles(p, 1, 4 * UNIT, steps_len=7, bias=0.5)
    vS = marbles(p, 1, 7 * UNIT, steps_len=8, bias=0.6)
    eH = env(p, 1, 0.0, 0.7, 0.0, 0.8)
    eV = env(p, 1, 0.0, 0.68, 0.0, 0.78)
    eS = env(p, 1, 0.0, 0.6, 0.0, 0.7)
    mA = lfo(p, 1, 215.0)
    mB = lfo(p, 1, 77.0)
    vt = vert(p, 1, [0.1, 0.1, 0.1, 0.1, 0.6, 0.05, 0.3, 0])
    vv = vert(p, 1, [0.55, 0.5, 0.45, 0.4, 0.5, 0.35, 0.3, 0.4])
    pr = probe(p, 1, fo)
    p.notes(2, "70s IV THREE COILS\n"
            "The colour footage, enlarged to fill the raster, drawn on a Deflection Yoke. Its three coils are three "
            "voices, each with its own Marbles at 3, 4 and 7 beats to every ten seconds: H bends the picture "
            "sideways, V bends it up and down, S twists it. Each voice strikes its coil on its own beats (a snap with "
            "a slow recovery) and loops its own pattern of coil speeds, line counts and twists; when the three beats "
            "coincide the picture tears at once, between them it ripples in turns.\n"
            "The H voice also flips the picture left to right on some beats; the V voice flips it upside down.\n"
            "The 215 s macro locks and loosens every loop (and with them the pace); the 77 s macro deepens the yoke "
            "and lengthens the phosphor trails. The probe reads motion: a busy picture curls the lines.\n"
            "Play: Yoke DEPTH, any voice's DEJA VU, Yoke LINES.")
    p.cable(vH, T1, eH, 4, RED); p.cable(vV, T1, eV, 4, RED); p.cable(vS, T1, eS, 4, RED)
    p.cable(eH, 0, vv, 0, YEL); p.cable(vv, 0, yk, 11, YEL)                       # H snap -> H coil amount
    p.cable(eV, 0, vv, 1, YEL); p.cable(vv, 1, yk, 12, YEL)                       # V snap -> V coil amount
    p.cable(eS, 0, vv, 2, YEL); p.cable(vv, 2, yk, 13, YEL)                       # S snap -> S coil amount
    p.cable(vH, X1, yk, 4, GREEN)                                                 # H coil rate (1 V/oct)
    p.cable(vV, X1, yk, 8, GREEN)                                                 # V coil rate
    p.cable(vS, X1, yk, 9, GREEN)                                                 # S coil rate
    p.cable(vV, X2, vv, 3, GREEN); p.cable(vv, 3, yk, 22, GREEN)                  # V: line count
    p.cable(vS, X2, vv, 4, GREEN); p.cable(vv, 4, yk, 19, GREEN)                  # S: twist
    p.cable(vH, X2, vv, 5, GREEN); p.cable(vv, 5, cl, 1, GREEN)                   # H: hue drift
    p.cable(vH, T3, yk, 27, RED)                                                  # H: flip horizontal
    p.cable(vV, T3, yk, 28, RED)                                                  # V: flip vertical
    p.cable(vS, T3, yk, 5, RED)                                                   # S: coil phase reset
    p.cable(mA, SIN, vt, 0, YEL)
    p.cable(vt, 0, vH, I_DV, YEL); p.cable(vt, 1, vV, I_DV, YEL); p.cable(vt, 2, vS, I_DV, YEL)
    p.cable(mB, TRI, vt, 4, YEL); p.cable(vt, 4, yk, 3, YEL)                      # macro B -> DEPTH
    p.cable(vt, 5, yk, 25, YEL)                                                   #   -> PERSIST
    p.cable(pr, P_MOTION, vv, 7, BLUE); p.cable(vv, 7, yk, 18, BLUE)              # motion -> CURL
    p.cable(mA, TRI, vt, 6, YEL); p.cable(vt, 6, vv, 6, YEL)                      # (spare)
    return p


# -------------------------------------------------------------------------------------------------
# 5. Echo relief: the present picture drawn on the relief of a past one; three voices set how far back.
def echo_relief():
    p = Patch()
    pl = player(p, 0)
    ws = p.add(VRU, "WaveShaper", 0, dict(SPHERE_WS))
    fb = p.add(VRU, "Feedback", 0, dict(SPHERE_FB))
    ro = p.add(VRU, "Rotator", 0, {2: 0.4, 9: 1, 10: 0.6})
    fd = p.add(VRU, "FrameDelay", 0, {0: 30.0, 1: 1.0})
    sp = p.add(VRU, "ScanProcessor", 0, {**SPHERE_SP, 8: 1})                     # colour from source
    cl = p.add(VRU, "ColorLab", 0, {**SPHERE_CL, 1: 0.9})
    dp = p.add(VRU, "Display", 0)
    chain(p, [pl, ws, fb, ro, sp])
    p.cable(ro, 0, fd, 0)
    p.cable(fd, 0, sp, 9, BLUE)                                                   # the past lifts the lines
    p.cable(sp, 1, cl, 0); p.cable(cl, 0, dp, 0)

    v1 = marbles(p, 1, 3 * UNIT, steps_len=5)                                     # relief voice
    v2 = marbles(p, 1, 4 * UNIT, steps_len=7)                                     # memory voice
    v3 = marbles(p, 1, 7 * UNIT, steps_len=8, bias=0.65)                          # surface voice
    e1 = env(p, 1, 0.0, 0.72, 0.0, 0.78)
    e3 = env(p, 1, 0.0, 0.6, 0.0, 0.66)
    mA = lfo(p, 1, 195.0)
    mB = lfo(p, 1, 73.0)
    vt = vert(p, 1, [0.1, 0.1, 0.1, 0.1, 0.25, 0.3, 0, 0])
    vv = vert(p, 1, [0.6, 1.0, 0.15, 0.04, 0.35, 0.4, 0.5, 0.5])
    pr = probe(p, 1, cl)
    p.notes(2, "70s IV ECHO RELIEF\n"
            "The sphere picture (contours, echo, turned) drawn as a raised scan relief, but the height of each line "
            "comes from the same picture some frames ago: the present is drawn on the shape of the past.\n"
            "Three Marbles at 3, 4 and 7 beats to every ten seconds. The relief voice sets how high the lines stand "
            "and strikes the echo's zoom; the memory voice loops how far back the relief reaches (a few frames to a "
            "second) and the echo hue; the surface voice nudges the raster and turns the picture a notch.\n"
            "The 195 s macro locks and loosens the loops; the 73 s macro swings the colour and the line brightness. "
            "The probe reads motion: a busy picture keys the echo away.\n"
            "Play: Frame Delay FRAMES, Scan Processor LUMA GAIN, relief Marbles DEJA VU.")
    p.cable(v1, X1, vv, 0, GREEN); p.cable(vv, 0, sp, 1, GREEN)                   # relief height
    p.cable(v1, T1, e1, 4, RED)
    p.cable(e1, 0, vv, 4, YEL); p.cable(vv, 4, fb, 1, YEL)                        # strike -> echo ZOOM
    p.cable(v2, X2, vv, 1, GREEN); p.cable(vv, 1, fd, 1, GREEN)                   # how far back
    p.cable(v2, X1, vv, 2, GREEN); p.cable(vv, 2, fb, 5, GREEN)                   # echo hue
    p.cable(v3, X1, vv, 3, GREEN); p.cable(vv, 3, sp, 6, GREEN)                   # raster nudge H
    p.cable(v3, T1, ro, 6, RED)                                                   # notch turn
    p.cable(v3, T3, e3, 4, RED)
    p.cable(e3, 0, vv, 5, YEL); p.cable(vv, 5, ws, 3, YEL)                        # strike -> contour count
    p.cable(mA, SIN, vt, 0, YEL)
    p.cable(vt, 0, v1, I_DV, YEL); p.cable(vt, 1, v2, I_DV, YEL); p.cable(vt, 2, v3, I_DV, YEL)
    p.cable(mB, TRI, vt, 4, YEL); p.cable(vt, 4, cl, 1, YEL)                      # macro B -> hue
    p.cable(vt, 5, sp, 5, YEL)                                                    #   -> line INTENSITY
    p.cable(pr, P_MOTION, vv, 6, BLUE); p.cable(vv, 6, fb, 7, BLUE)               # motion -> echo KEY
    return p


PATCHES = [("70s IV four voices.vcv", four_voices), ("70s IV wipe round.vcv", wipe_round),
           ("70s IV key ladder.vcv", key_ladder), ("70s IV three coils.vcv", three_coils),
           ("70s IV echo relief.vcv", echo_relief)]

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    only = sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    for name, fn in PATCHES:
        if only and not any(o in name for o in only):
            continue
        print(Patch.save(fn(), out, name))
