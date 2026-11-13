#!/usr/bin/env python3
"""Write the demo patches in patches/. Parameter and port ids come from the module sources
(src/*.cpp) and the Fundamental 2.6.4 sources; check them with scripts/check-patches.py."""
import json, math, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VRU, FUND, CORE = "VideoResearchUnit", "Fundamental", "Core"
VER = {VRU: "2.0.0", FUND: "2.6.4", CORE: "2.6.6"}
# Rack's default cable colours.
RED, YEL, GREEN, BLUE, PURPLE = "#f3374b", "#ffb437", "#00b56e", "#3695ef", "#8b4ade"
HP = {"ScopeTrace": 20, "VideoOsc": 20, "BusMixer": 8, "Colorizer": 12, "Display": 30, "ScanProcessor": 13,
      "Shapes": 14, "Rotator": 14, "Octoscillator": 36, "Feedback": 12, "DeflectionYoke": 20,
      "DualLfoChaos": 8, "VCO": 9, "LFO": 9, "VCA": 5, "8vert": 8, "VideoPlayer": 12,
      "AudioInterface2": 12, "Notes": 16}

def hz_norm(hz):  # Dual LFO Chaos rate knob: exponential, 0.01 to 20 Hz
    return math.log(hz / 0.01) / math.log(2000.0)

class Patch:
    def __init__(self):
        self.mods, self.cables = [], []
    def add(self, plugin, model, x, y, params=None, data=None):
        m = {"id": len(self.mods) + 1, "plugin": plugin, "model": model, "version": VER[plugin],
             "params": [{"id": k, "value": v} for k, v in sorted((params or {}).items())], "pos": [x, y]}
        if data is not None:
            m["data"] = data
        self.mods.append(m)
        return m["id"]
    def cable(self, out_mod, out_id, in_mod, in_id, color):
        self.cables.append({"id": len(self.cables) + 1, "outputModuleId": out_mod, "outputId": out_id,
                            "inputModuleId": in_mod, "inputId": in_id, "color": color})
    def save(self, name):
        doc = {"version": "2.6.6", "zoom": 1.0, "gridOffset": [0.0, 0.0],
               "modules": self.mods, "cables": self.cables}
        with open(os.path.join(ROOT, "patches", name), "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")

def notes(p, x, y, text):
    return p.add(CORE, "Notes", x, y, data={"text": text})

# ---------------------------------------------------------------- 1. Scope Trace Lissajous
def lissajous():
    p = Patch()
    st = p.add(VRU, "ScopeTrace", 0, 0, {0: 0, 1: 0.85, 2: 0.85, 5: 0.32, 6: 0.42, 7: 0.72, 8: 0.32,
                                        9: 1.25, 21: 0.3, 22: 0.5, 14: 1.0})
    dp = p.add(VRU, "Display", 20, 0)
    notes(p, 50, 0, "Two sine oscillators a fifth apart draw a Lissajous figure on the Scope Trace.\n"
          "A slow LFO detunes the second oscillator a little, so the figure keeps turning inside out.\n"
          "A second LFO swells the beam dwell, and a slow saw turns the whole picture.\n"
          "Try the Scope Trace knobs: DECAY, BEAM, BLOOM and HUE.\n"
          "Try the frequency knob on the first oscillator. A whole number ratio like 3 to 2 makes a still figure.\n"
          "The oscillators also play quietly through the audio output.")
    va = p.add(FUND, "VCO", 0, 1, {2: -12.0})
    vb = p.add(FUND, "VCO", 9, 1, {2: -5.0, 4: 0.001})
    l1 = p.add(FUND, "LFO", 18, 1, {0: 0, 2: -3.5})
    l2 = p.add(FUND, "LFO", 27, 1, {0: 1, 2: -3.0})
    l3 = p.add(FUND, "LFO", 36, 1, {0: 1, 2: -4.3})
    va_ = p.add(FUND, "VCA", 45, 1, {0: 0.2, 1: 0.2})
    au = p.add(CORE, "AudioInterface2", 50, 1)
    p.cable(va, 0, st, 0, YEL); p.cable(vb, 0, st, 1, YEL)
    p.cable(st, 0, dp, 0, BLUE)
    p.cable(l1, 0, vb, 1, GREEN)
    p.cable(l2, 0, st, 12, GREEN)
    p.cable(l3, 2, st, 8, GREEN)
    p.cable(va, 0, va_, 2, RED); p.cable(vb, 0, va_, 5, RED)
    p.cable(va_, 0, au, 0, RED); p.cable(va_, 1, au, 1, RED)
    p.save("1 - Scope Trace Lissajous.vcv")

# ---------------------------------------------------------------- 2. Video Oscillators
def oscillators():
    p = Patch()
    a = p.add(VRU, "VideoOsc", 0, 0, {0: 3.0, 1: 0.5, 2: 0.3, 3: 0.5})
    b = p.add(VRU, "VideoOsc", 20, 0, {0: 5.0, 1: 0.05, 2: 0.2, 3: 0.25})
    mx = p.add(VRU, "BusMixer", 40, 0, {0: 0.5, 1: 4})
    co = p.add(VRU, "Colorizer", 48, 0)
    dp = p.add(VRU, "Display", 0, 1)
    lf = p.add(VRU, "DualLfoChaos", 30, 1, {0: hz_norm(0.11), 1: 0.25, 2: hz_norm(0.07), 3: 0.25})
    sl = p.add(FUND, "LFO", 38, 1, {0: 0, 2: -5.0})
    notes(p, 47, 1, "Two video oscillators are mixed with the Bus Mixer in Difference mode. "
          "Where the two patterns disagree you see moire stripes.\n"
          "The Colorizer paints the grey stripes with colour.\n"
          "Slow LFOs move the stripe frequencies and shift the colours, so the picture never stops flowing.\n"
          "Try the MIX knob and the blend MODE on the Bus Mixer.\n"
          "Try the FREQ knobs on the oscillators and the HUE knobs on the Colorizer.")
    p.cable(a, 0, mx, 0, PURPLE); p.cable(b, 0, mx, 1, PURPLE)
    p.cable(mx, 0, co, 0, PURPLE); p.cable(co, 0, dp, 0, BLUE)
    p.cable(lf, 0, a, 0, YEL)          # LFO A -> oscillator A frequency
    p.cable(lf, 1, b, 0, YEL)          # LFO B -> oscillator B frequency
    p.cable(lf, 3, co, 7, GREEN)       # chaos Y -> Colorizer offset
    p.cable(sl, 0, a, 1, GREEN)        # slow sine -> oscillator A hue
    p.save("2 - Video Oscillator Moire.vcv")

# ---------------------------------------------------------------- 3. Feedback tunnel
def tunnel():
    p = Patch()
    sh = p.add(VRU, "Shapes", 0, 0, {0: 5.0, 1: 0.2, 2: 0.35, 3: 0.3, 12: 0.2, 13: 1})
    fb = p.add(VRU, "Feedback", 14, 0, {0: -0.1, 1: 0.01, 4: 0.025, 5: 0.85})
    wb = p.add(VRU, "DeflectionYoke", 26, 0, {0: 0.85, 2: 0.55, 3: 0.5666123317651051, 5: 0.25,
                                        9: 0.5666123317651051, 12: 0.514678753773118, 13: 2.0, 14: 240.0,
                                        15: 0.2, 16: 0.646240625180289, 17: 0.35, 18: 0.4,
                                        20: 0.5666123317651051, 21: 1.0, 22: 1.0})
    dp = p.add(VRU, "Display", 0, 1)
    lf = p.add(VRU, "DualLfoChaos", 46, 1, {0: hz_norm(0.07), 1: 0.06, 2: hz_norm(0.043), 3: 0.3})
    l1 = p.add(FUND, "LFO", 54, 1, {0: 1, 2: -4.0})
    l2 = p.add(FUND, "LFO", 63, 1, {0: 1, 2: -4.7})
    va = p.add(FUND, "VCA", 72, 1, {0: 0.15, 1: 0.2})
    notes(p, 30, 1, "A white star is fed back into itself, and each pass shrinks, turns and shifts its colour a little. "
          "That makes a glowing tunnel.\n"
          "The Deflection Yoke then bends the picture like a wobbling monitor.\n"
          "Slow LFOs breathe the star's size and points, and sway the tunnel's turn and zoom.\n"
          "Try the Feedback knobs: GAIN, ZOOM, ROTATE and HUE per pass. Press the clear button after big changes.\n"
          "Try the Deflection Yoke DEPTH and RATE knobs.")
    p.cable(sh, 0, fb, 0, PURPLE); p.cable(fb, 0, wb, 0, PURPLE); p.cable(wb, 0, dp, 0, BLUE)
    p.cable(lf, 0, fb, 2, YEL)         # LFO A -> Feedback rotate
    p.cable(lf, 1, fb, 1, YEL)         # LFO B -> Feedback zoom
    p.cable(l1, 0, va, 2, GREEN); p.cable(va, 0, sh, 3, GREEN)   # slow sine, softened -> Shapes size
    p.cable(l2, 0, va, 5, GREEN); p.cable(va, 1, sh, 2, GREEN)   # slow sine, softened -> Shapes point
    p.save("3 - Feedback Tunnel.vcv")

# ---------------------------------------------------------------- 4. Slow Scan Processor
def slow_scan():
    p = Patch()
    vo = p.add(VRU, "VideoOsc", 0, 0, {0: 2.5, 1: 0.5, 2: 0.35})
    sh = p.add(VRU, "Shapes", 20, 0, {0: 6.0, 1: 0.6, 3: 0.4, 12: 0.8})
    mx = p.add(VRU, "BusMixer", 34, 0, {0: 0.5, 1: 0})
    re = p.add(VRU, "ScanProcessor", 42, 0, {0: 0.3, 3: 0.1, 4: 0.8, 5: 1, 6: 0.8, 8: 1, 9: 0.5, 10: 0.5, 11: 0.4})
    dp = p.add(VRU, "Display", 0, 1)
    lf = p.add(VRU, "DualLfoChaos", 30, 1, {0: hz_norm(0.01), 1: 0.08, 2: hz_norm(0.0137), 3: 0.06})
    l1 = p.add(FUND, "LFO", 38, 1, {0: 0, 2: -7.6})    # 193 s
    l2 = p.add(FUND, "LFO", 47, 1, {0: 0, 2: -6.6})    # 98 s
    l3 = p.add(FUND, "LFO", 56, 1, {0: 0, 2: -6.0})    # 64 s
    l4 = p.add(FUND, "LFO", 65, 1, {0: 0, 2: -5.4})    # 42 s
    # 8vert (attenuverter) rows: each LFO is turned down before it reaches a destination with no attenuverter.
    av = p.add(FUND, "8vert", 74, 1, {0: 0.15, 1: 0.2, 2: 0.5})
    notes(p, 55, 0, "This patch is slow on purpose. Leave it running and come back in ten minutes.\n"
          "Scan Processor draws the picture as bright lines that are pushed up by how bright it is underneath.\n"
          "The picture underneath is a video oscillator mixed with a soft grey shape.\n"
          "Six very slow LFOs, with cycles between 40 seconds and 3 minutes, bend the lines, the stripes, "
          "the shape and the colour at unrelated paces, so the same view rarely comes back.\n"
          "Try LUMA GAIN, INTENSITY and PERSIST on the Scan Processor, or turn up the depth on the Dual LFO Chaos.")
    p.cable(vo, 0, mx, 0, PURPLE); p.cable(sh, 0, mx, 1, PURPLE)
    p.cable(mx, 0, re, 0, PURPLE); p.cable(re, 0, dp, 0, BLUE)
    p.cable(l1, 0, vo, 1, GREEN)                         # hue sweeps the whole range
    p.cable(l3, 1, av, 2, YEL); p.cable(av, 2, vo, 0, YEL)   # triangle x 0.5 -> oscillator frequency, +-1
    p.cable(l2, 0, av, 0, YEL); p.cable(av, 0, re, 1, YEL)   # sine x 0.15 -> luma gain, 0.15 to 0.45
    p.cable(l4, 0, av, 1, YEL); p.cable(av, 1, sh, 3, YEL)   # sine x 0.2 -> shape size, 0.3 to 0.5
    p.cable(lf, 0, re, 7, GREEN)                         # slow drift -> vertical position
    p.cable(lf, 1, re, 6, GREEN)                         # slow drift -> horizontal position
    p.save("4 - Slow Scan Processor.vcv")

# ---------------------------------------------------------------- 5. Octoscillator garden
def garden():
    p = Patch()
    sh = p.add(VRU, "Shapes", 0, 0, {0: 4.0, 1: 0.9, 3: 0.3, 8: 1, 9: 6, 10: 0.5, 11: 0.55, 12: 0.5})
    ro = p.add(VRU, "Rotator", 14, 0, {1: 0.06, 2: 0.12, 7: 3, 8: 6, 9: 1, 10: 0.25})
    co = p.add(VRU, "Colorizer", 28, 0)
    dp = p.add(VRU, "Display", 40, 0)
    # Oscillator 3 drives SIZE, which dies away near the bottom of its knob, so its own DEPTH is turned down.
    oc = p.add(VRU, "Octoscillator", 0, 1, {0: -1.0, 3: 0.6, 15: 0.25})
    notes(p, 36, 1, "A ring of soft shapes melts and turns while a kaleidoscope folds them over.\n"
          "The Octoscillator is a set of slow chaotic voltages, and it moves the shape's sides, melt and size, "
          "the kaleidoscope's angle and zoom, and the colours.\n"
          "Nothing repeats, so leave it running.\n"
          "Try the SPEED knob on the Octoscillator, and its LEVEL knob for bigger moves.\n"
          "Try FOLDS on the Rotator and BANDS on the Colorizer.")
    p.cable(sh, 0, ro, 0, PURPLE); p.cable(ro, 0, co, 0, PURPLE); p.cable(co, 0, dp, 0, BLUE)
    p.cable(oc, 0, sh, 0, GREEN)       # osc 1 X -> sides
    p.cable(oc, 3, sh, 8, GREEN)       # osc 2 X -> melt
    p.cable(oc, 6, sh, 3, GREEN)       # osc 3 X -> size
    p.cable(oc, 9, ro, 1, GREEN)       # osc 4 X -> kaleidoscope angle
    p.cable(oc, 13, ro, 3, GREEN)      # osc 5 Y -> kaleidoscope zoom
    p.cable(oc, 15, co, 7, GREEN)      # osc 6 X -> colour offset
    p.cable(oc, 18, sh, 7, GREEN)      # osc 7 X -> shape turn
    p.save("5 - Octoscillator Garden.vcv")

# ---------------------------------------------------------------- 6. Muybridge on the Scan Processor
def muybridge():
    p = Patch()
    vp = p.add(VRU, "VideoPlayer", 0, 0, {3: 1})                       # LOOP on
    re = p.add(VRU, "ScanProcessor", 12, 0, {0: 0.3, 3: 0.1, 4: 1.0, 5: 1, 6: 1.2, 9: 0.6, 10: 0.4})
    dp = p.add(VRU, "Display", 25, 0)
    l1 = p.add(FUND, "LFO", 0, 1, {0: 0, 2: -5.0})                      # bipolar, 31 s
    l2 = p.add(FUND, "LFO", 9, 1, {0: 0, 2: -4.3})                      # bipolar, 51 s
    # Scan Processor takes +-5 V as +-1 on LUMA GAIN and 10 V as its whole INTENSITY travel, so a full LFO
    # would fling the picture off the top and then black it out. 8vert turns each LFO down first.
    av = p.add(FUND, "8vert", 18, 1, {0: 0.15, 1: 0.27})
    notes(p, 26, 1, "Muybridge, The Horse in Motion (1878): the first motion study, run through a Scan Processor-style scan processor.\n"
          "This patch needs a clip, and the Video Player starts empty. The demo clip is a separate download on the Video Research Unit release page (the demos zip, in its clip folder). "
          "It is also on Wikimedia Commons: https://commons.wikimedia.org/wiki/File:Muybridge_race_horse_animated.gif (public domain; that file is a GIF, so convert it to MP4 first).\n"
          "Any video file works. Right-click the Video Player, choose Open video file..., then press PLAY. LOOP is already on.\n"
          "Try this: turn LUMA GAIN and INTENSITY on the Scan Processor, change Scanlines, or switch on ROTATE 90. "
          "Two slow LFOs already move LUMA GAIN (between 0.15 and 0.45) and INTENSITY (between 0.8 and 1.6), turned down by the 8vert so the horse stays in frame and lit.")
    p.cable(vp, 0, re, 0, PURPLE); p.cable(re, 0, dp, 0, BLUE)
    p.cable(l1, 0, av, 0, YEL); p.cable(av, 0, re, 1, YEL)   # sine x 0.15 -> luma gain
    p.cable(l2, 0, av, 1, GREEN); p.cable(av, 1, re, 5, GREEN)   # sine x 0.27 -> intensity
    p.save("6 - Muybridge on the Scan Processor.vcv")

if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "patches"), exist_ok=True)
    lissajous(); oscillators(); tunnel(); slow_scan(); garden(); muybridge()
