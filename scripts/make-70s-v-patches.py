#!/usr/bin/env python3
"""Write one self-playing studio patch ("70s V slow tide"), built for a smooth picture.

Sets III and IV layered several full pictures per frame (four voices into a Video Mixer), which costs more than
one screen refresh at 1080 lines. This one goes back to the first set's economy: ONE footage source and ONE
chain of treatments, each working on the last one's picture, so the Display draws a single picture per frame.
It keeps what made the later sets play themselves:
  - independent clocks: three LFOs at 13, 17 and 31 seconds, which never line up, each moving one treatment;
  - a Marbles that loops a short, slowly changing pattern of turns and echo lengths;
  - a slow macro (about four minutes) and a second one (97 s), so the piece has sections;
  - the film plays the patch: a Video Probe watches the footage; movement in it strikes an envelope that pushes
    the trails outward, and bright film keys through the trails.
Timed with tools/vru_render_patch.cpp (--thread-mode --hash): a contour stage (Wave Shaper) and a Color Lab were
cut because they pushed a 720-line frame past the 10 ms budget.
The Display is set to 720 lines (data "pictureLines").

Ids checked against source: VRU main (dc61b13) FrameDelay, Feedback, Rotator, VideoProbe,
Display; Fundamental v2.6.4 LFO, ADSR, 8vert; AudibleInstruments 2.0.0 Marbles.

Usage: scripts/make-70s-v-patches.py [output-dir] [name ...]   (default output: the repo's patches/)"""
import importlib.util, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(fname, modname):
    spec = importlib.util.spec_from_file_location(modname, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


m4 = _load("make-70s-iv-patches.py", "m4")                      # also loads the first and third sets' helpers
Patch, VRU, FUND = m4.Patch, m4.VRU, m4.FUND
RED, YEL, GREEN, BLUE, PURPLE = m4.RED, m4.YEL, m4.GREEN, m4.BLUE, m4.PURPLE
player, chain, lfo, vert, probe, marbles, env = m4.player, m4.chain, m4.lfo, m4.vert, m4.probe, m4.marbles, m4.env
SIN, TRI, SAW, SQR = m4.SIN, m4.TRI, m4.SAW, m4.SQR
P_AVG, P_MOTION, P_GATE = m4.P_AVG, m4.P_MOTION, m4.P_GATE
T1, T2, T3, X1, X2, X3, I_DV = m4.T1, m4.T2, m4.T3, m4.X1, m4.X2, m4.X3, m4.I_DV
OUT = os.path.join(os.path.dirname(HERE), "patches")

# CV inputs on the chain
FD_FRAMES = 1                                                   # FrameDelay FRAMES
FB_ZOOM, FB_ROT, FB_X, FB_Y, FB_HUE, FB_GAIN, FB_KEY = 1, 2, 3, 4, 5, 6, 7   # Feedback
RO_SPIN, RO_ZOOM, RO_NOTCH = 2, 3, 6                            # Rotator


# -------------------------------------------------------------------------------------------------
def slow_tide():
    p = Patch()
    pl = player(p, 0)
    fd = p.add(VRU, "FrameDelay", 0, {0: 12.0, 1: 1.0})                                   # a moment ago
    fb = p.add(VRU, "Feedback", 0, {0: 0.05, 1: 0.0, 4: 0.0, 5: 0.62, 6: 0.25, 8: 0, 9: 1})  # trails, blend
    ro = p.add(VRU, "Rotator", 0, {2: 0.4, 7: 3, 8: 1, 9: 2, 10: 0.9})                    # fill, mirrored edges, 1/8 notch
    dp = p.add(VRU, "Display", 0, data={"pictureLines": 720})
    chain(p, [pl, fd, fb, ro])
    p.cable(ro, 0, dp, 0)

    c1 = lfo(p, 1, 13.0, uni=False)                             # three clocks that never line up
    c2 = lfo(p, 1, 17.0, uni=False)
    c3 = lfo(p, 1, 31.0, uni=False)
    mA = lfo(p, 1, 230.0, uni=False)                            # sections: about four minutes
    mB = lfo(p, 1, 97.0, uni=False)
    mb = marbles(p, 1, 0.33, steps_len=5, dv=0.42, bias=0.35, spread=0.55)
    hit = env(p, 1, 0.05, 0.62, 0.0, 0.62)                      # quick rise, a second or so to fall back
    va = vert(p, 1, [0.12, 0.25, 0.12, 0.4, 0.2, 0.4, 0.3, 0.0])
    vb = vert(p, 1, [0.15, 0.3, 0.5, 0.15, 0.0, 0.0, 0.0, 0.0])
    pr = probe(p, 1, pl)                                        # listens to the film itself: cheap to watch
    p.notes(2, "70s V SLOW TIDE\n"
            "One film, one chain: a moment-ago copy (Frame Delay), trails (Feedback), a slowly turning picture with "
            "mirrored edges (Rotator). The Display draws one picture per frame, set to 720 lines, so it plays "
            "smoothly.\n"
            "Three LFOs at 13, 17 and 31 seconds never line up: they drift the trails sideways, twist them, and lift "
            "them. A Marbles loops five steps that slowly change: each beat may turn the picture a notch, and it sets "
            "how far back the copy is and the trail colour.\n"
            "The four-minute LFO makes the sections: clean film, then long trails, then back; it also locks and "
            "loosens the Marbles loop. The 97 s LFO sets the picture spinning and breathing in and out.\n"
            "The film plays the patch: Video Probe watches it; movement fires the envelope, which pushes the trails "
            "outward; bright film cuts through the trails.\n"
            "Play: Feedback GAIN (trail length), Marbles DEJA VU (repeat or wander), Rotator ZOOM.")
    # clocks
    p.cable(c1, SIN, va, 0, GREEN); p.cable(va, 0, fb, FB_X, GREEN)                  # 13 s: trails drift sideways
    p.cable(c2, TRI, va, 1, GREEN); p.cable(va, 1, fb, FB_ROT, GREEN)                # 17 s: trails twist
    p.cable(c3, SIN, va, 2, GREEN); p.cable(va, 2, fb, FB_Y, GREEN)                  # 31 s: trails lift and sink
    # Marbles: the short loop
    p.cable(mb, T1, ro, RO_NOTCH, RED)                                               # a beat turns the picture
    p.cable(mb, X1, va, 3, GREEN); p.cable(va, 3, fd, FD_FRAMES, GREEN)              # how far back the copy is
    p.cable(mb, X2, va, 4, GREEN); p.cable(va, 4, fb, FB_HUE, GREEN)                 # trail colour per pass
    # sections
    p.cable(mA, TRI, va, 5, YEL); p.cable(va, 5, fb, FB_GAIN, YEL)                   # trail length
    p.cable(mA, SIN, vb, 3, YEL); p.cable(vb, 3, mb, I_DV, YEL)                      # loop locks and loosens
    p.cable(mB, SIN, vb, 2, YEL); p.cable(vb, 2, ro, RO_SPIN, YEL)                   # picture drifts round
    p.cable(mB, TRI, va, 6, YEL); p.cable(va, 6, ro, RO_ZOOM, YEL)                   # and breathes in and out
    # the film plays the patch
    p.cable(pr, P_GATE, hit, 4, RED)                                                 # movement strikes
    p.cable(hit, 0, vb, 0, YEL); p.cable(vb, 0, fb, FB_ZOOM, YEL)                    #   trails surge outward
    p.cable(pr, P_AVG, vb, 1, BLUE); p.cable(vb, 1, fb, FB_KEY, BLUE)                # bright film cuts through
    return p


PATCHES = [("70s V slow tide.vcv", slow_tide)]

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    only = sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    for name, fn in PATCHES:
        if only and not any(o in name for o in only):
            continue
        print(Patch.save(fn(), out, name))
