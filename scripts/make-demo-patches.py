#!/usr/bin/env python3
"""Write the demo patches in patches/. Parameter and port ids come from the module sources
(src/*.cpp) and the Fundamental 2.6.4 sources; check them with scripts/check-patches.py."""
import json, math, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VRU, FUND, CORE = "VideoResearchUnit", "Fundamental", "Core"
VER = {VRU: "2.0.0", FUND: "2.6.4", CORE: "2.6.6"}
# Rack's default cable colours.
RED, YEL, GREEN, BLUE, PURPLE = "#f3374b", "#ffb437", "#00b56e", "#3695ef", "#8b4ade"
HP = {"ScopeTrace": 20, "VideoOsc": 29, "BusMixer": 8, "Colorizer": 12, "Display": 30, "ScanProcessor": 13,
      "Shapes": 14, "VideoProbe": 20, "Raster": 10, "Ramps": 14, "Rotator": 14, "VideoMixer": 30, "FrameDelay": 7, "Octoscillator": 36, "Feedback": 12, "DeflectionYoke": 20,
      "DualLfoChaos": 8, "VCO": 9, "LFO": 9, "VCA": 5, "8vert": 8, "VideoPlayer": 12,
      "AudioInterface2": 12, "Notes": 16, "ScanSync": 10}

def hz_norm(hz):  # Dual LFO Chaos rate knob: exponential, 0.01 to 20 Hz
    return math.log(hz / 0.01) / math.log(2000.0)

class Patch:
    def __init__(self, vru_version=None):
        self.mods, self.cables = [], []
        self.vru_version = vru_version
    def add(self, plugin, model, x, y, params=None, data=None):
        m = {"id": len(self.mods) + 1, "plugin": plugin, "model": model, "version": (self.vru_version if plugin == VRU and self.vru_version else VER[plugin]),
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
    b = p.add(VRU, "VideoOsc", 30, 0, {0: 5.0, 1: 0.05, 2: 0.2, 3: 0.25})
    mx = p.add(VRU, "BusMixer", 60, 0, {0: 0.5, 1: 4})
    co = p.add(VRU, "Colorizer", 68, 0)
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
    sh = p.add(VRU, "Shapes", 30, 0, {0: 6.0, 1: 0.6, 3: 0.4, 12: 0.8})
    mx = p.add(VRU, "BusMixer", 44, 0, {0: 0.5, 1: 0})
    re = p.add(VRU, "ScanProcessor", 52, 0, {0: 0.3, 3: 0.1, 4: 0.8, 5: 1, 6: 0.8, 8: 1, 9: 0.5, 10: 0.5, 11: 0.4})
    dp = p.add(VRU, "Display", 0, 1)
    lf = p.add(VRU, "DualLfoChaos", 30, 1, {0: hz_norm(0.01), 1: 0.08, 2: hz_norm(0.0137), 3: 0.06})
    l1 = p.add(FUND, "LFO", 38, 1, {0: 0, 2: -7.6})    # 193 s
    l2 = p.add(FUND, "LFO", 47, 1, {0: 0, 2: -6.6})    # 98 s
    l3 = p.add(FUND, "LFO", 56, 1, {0: 0, 2: -6.0})    # 64 s
    l4 = p.add(FUND, "LFO", 65, 1, {0: 0, 2: -5.4})    # 42 s
    # 8vert (attenuverter) rows: each LFO is turned down before it reaches a destination with no attenuverter.
    av = p.add(FUND, "8vert", 74, 1, {0: 0.15, 1: 0.2, 2: 0.5})
    notes(p, 65, 0, "This patch is slow on purpose. Leave it running and come back in ten minutes.\n"
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

# ---------------------------------------------------------------- 7. Loop through a delay
def delay_loop():
    p = Patch()
    # A white matte pentagon. Two slow LFOs, turned down by a VCA, walk it round the middle of the picture.
    sh = p.add(VRU, "Shapes", 0, 0, {0: 5.0, 1: 0.25, 3: 0.2, 13: 1})
    vm = p.add(VRU, "VideoMixer", 14, 0, {10: 0.45, 15: 1})           # FADER 0.45 and ADD: A in full, B at 90%
    ro = p.add(VRU, "Rotator", 44, 0, {0: 0.012, 2: -0.012})           # a little turn and a little shrink on every pass
    fd = p.add(VRU, "FrameDelay", 58, 0, {0: 3.0})                     # three frames
    dp = p.add(VRU, "Display", 0, 1)
    l1 = p.add(FUND, "LFO", 30, 1, {0: 0, 2: -3.3})
    l2 = p.add(FUND, "LFO", 39, 1, {0: 0, 2: -3.0})
    va = p.add(FUND, "VCA", 48, 1, {0: 0.5, 1: 0.5})
    notes(p, 54, 1, "A shape is cabled round a loop: the Video Mixer feeds the Rotator, the Rotator feeds the Frame Delay, "
          "and the Frame Delay comes back into the mixer's second input. Without the delay this loop would be an error. "
          "With it, every picture is built from the last one, three frames old, turned and shrunk a little, "
          "so the shape leaves a spiralling trail.\n"
          "Two slow LFOs walk the shape around, so the trail keeps changing.\n"
          "Try FRAMES on the Frame Delay: longer makes a wider, more broken trail.\n"
          "Try the Video Mixer FADER: higher keeps the trail longer, and near the top it fills the picture. "
          "Try ANGLE and ZOOM on the Rotator.\n"
          "The Display shows the mixer. A second Display on the Frame Delay would show the same trail, three frames late.")
    p.cable(sh, 0, vm, 0, PURPLE)       # shape -> mixer A
    p.cable(vm, 0, dp, 0, BLUE)         # mixer -> Display
    p.cable(vm, 0, ro, 0, PURPLE)       # mixer -> Rotator
    p.cable(ro, 0, fd, 0, PURPLE)       # Rotator -> Frame Delay
    p.cable(fd, 0, vm, 1, PURPLE)       # Frame Delay -> mixer B
    p.cable(l1, 0, va, 2, GREEN); p.cable(va, 0, sh, 4, GREEN)   # slow sine, turned down -> shape X
    p.cable(l2, 0, va, 5, GREEN); p.cable(va, 1, sh, 5, GREEN)   # a second one -> shape Y
    p.save("7 - Loop Through A Delay.vcv")

# ---------------------------------------------------------------- 8. One oscillator bends another
OSC1_FREQ = math.log2(2 * 3.004)     # oscillator 1, A on ROWS: 3.004 cycles down each field (the knob reads 0.5 x 2^knob)
OSC2_FREQ = math.log2(2 * 20.0)      # oscillator 2, A on BARS: 20 bars across the picture
def bend():
    p = Patch()
    # VideoOsc ids: 0 FREQ A, 1 HUE, 2 FB, 3 WAVE A, 4 PATTERN, 5 SAT, 10 RANGE A (1 bars, 0 rows), 12 LOCK A, 15 MIX, 17 FM AMT.
    # Oscillator 1 is the modulator: a grey picture of soft rows, no feedback, A alone.
    o1 = p.add(VRU, "VideoOsc", 0, 0, {0: OSC1_FREQ, 2: 0.0, 3: 0.0, 4: 6, 5: 0.0, 10: 0, 12: 0.8, 15: 0})
    # Oscillator 2 is bent by it: about 20 bars across, A alone, FM AMT well up.
    o2 = p.add(VRU, "VideoOsc", 30, 0, {0: OSC2_FREQ, 1: 0.58, 2: 0.0, 3: 0.0, 4: 6, 10: 1, 12: 1.0, 15: 0, 17: 0.5})
    co = p.add(VRU, "Colorizer", 60, 0)
    dp = p.add(VRU, "Display", 0, 1)
    notes(p, 31, 1, "One oscillator bends another inside the picture.\n"
          "Oscillator 1 (left) draws soft grey rows. Its picture goes into FM IN on oscillator 2. "
          "Oscillator 2 draws about 20 bars across the picture. Where oscillator 1 is bright, the bars bunch together. "
          "Where it is dark, they spread apart. So the bars bend with the shape of the rows.\n"
          "Oscillator 1's LOCK A is a little below full, so its rows drift and the bend rolls down the picture.\n"
          "Try FM AMT on oscillator 2: negative reverses which parts bunch. Try FREQ A on either oscillator, "
          "and LOCK A on oscillator 1 (all the way up stops the roll).\n"
          "The Colorizer paints the grey bars. The Display shows the Colorizer.")
    p.cable(o1, 0, o2, 10, PURPLE)      # oscillator 1 -> oscillator 2 FM IN
    p.cable(o2, 0, co, 0, PURPLE)       # oscillator 2 -> Colorizer
    p.cable(co, 0, dp, 0, BLUE)         # Colorizer -> Display
    p.save("8 - One Oscillator Bends Another.vcv")

# ---------------------------------------------------------------- 9. A picture through a signal and back
def raster_loop():
    p = Patch()
    # Shapes ids: 0 SIDES, 1 ROUND, 3 SIZE, 13 MATTE. Video Probe ids: 7 SCAN, 8 LINES, 9 SLOW. Raster ids: 0 LINES, 1 FILL, 2 SLOW.
    sh = p.add(VRU, "Shapes", 0, 0, {0: 5.0, 1: 0.25, 3: 0.35})
    pr = p.add(VRU, "VideoProbe", 14, 0, {7: 1.0, 8: 120.0, 9: 5.0})
    rs = p.add(VRU, "Raster", 34, 0, {0: 120.0, 1: 1.0, 2: 5.0})
    d1 = p.add(VRU, "Display", 0, 1)
    d2 = p.add(VRU, "Display", 30, 1)
    notes(p, 44, 0, "A picture becomes a signal and the signal becomes a picture again.\n"
          "The Video Probe has SCAN on and nothing in X or Y, so it reads the Shapes picture as a raster on the video clock: "
          "left to right, top to bottom, one picture at a time. Its LUMA output carries that scan as an ordinary 0 to 10 V signal. "
          "LUMA goes into BRIGHT on the Raster, which paints each sample where the clock says it belongs.\n"
          "Both are set to 120 lines and SLOW x32, and the settings must match.\n"
          "SLOW makes a picture take 32 fields of the video clock instead of one, so each line lasts 32 times longer and gets about 98 samples at 48 kHz "
          "instead of 3. A picture comes round every 0.3 seconds. The right-hand Display shows the shape being redrawn from the top down, about 98 columns wide. "
          "Sharper means slower: at SLOW x1 the same patch gives a picture 3 columns wide, 110 times a second. It is a way to hear and process a picture, not a full-resolution path.\n"
          "Try patching LUMA through other modules on its way to BRIGHT (a VCA, a filter, an inverting attenuator), "
          "try FILL on the Raster, and try 60 or 240 lines on both.")
    p.cable(sh, 0, pr, 0, PURPLE)       # Shapes -> Video Probe BUS IN
    p.cable(sh, 0, d1, 0, PURPLE)       # Shapes -> left Display
    p.cable(pr, 0, rs, 0, YEL)          # LUMA -> Raster BRIGHT
    p.cable(rs, 0, d2, 0, PURPLE)       # Raster -> right Display
    p.save("9 - A Picture Through A Signal.vcv")

# ---------------------------------------------------------------- VRU-2.3.0-test: everything new in 2.3.0, one row each
def test_230():
    p = Patch("2.3.0")
    # Row 0, picture to CV. A moving Bars and Rows picture goes into the Video Probe. A slow LFO moves the probe's X
    # across it. The probe's LUMA output drives the FREQ A CV of a second Video Oscillator, so its bars bunch and spread.
    l0 = p.add(FUND, "LFO", 0, 0, {0: 0, 2: -2.6})
    o1 = p.add(VRU, "VideoOsc", 9, 0, {0: math.log2(2 * 4.0), 2: 0.0, 3: 0.0, 4: 6, 10: 1, 12: 0.8, 15: 0})
    pr = p.add(VRU, "VideoProbe", 29, 0, {2: 0.2, 3: 0.1})
    o2 = p.add(VRU, "VideoOsc", 49, 0, {0: 0.6, 1: 0.58, 2: 0.0, 3: 0.0, 4: 6, 10: 1, 12: 1.0, 15: 0})
    d0 = p.add(VRU, "Display", 69, 0)
    p.cable(o1, 0, pr, 0, PURPLE)       # picture -> Probe BUS IN
    p.cable(l0, 0, pr, 1, GREEN)        # slow sine -> Probe X
    p.cable(pr, 0, o2, 0, YEL)          # Probe LUMA -> second oscillator FREQ A CV
    p.cable(o2, 0, d0, 0, BLUE)         # second oscillator -> Display
    # Row 1, a loop through a delay, as patch 7.
    sh = p.add(VRU, "Shapes", 0, 1, {0: 5.0, 1: 0.25, 3: 0.2, 13: 1})
    vm = p.add(VRU, "VideoMixer", 14, 1, {10: 0.45, 15: 1})
    ro = p.add(VRU, "Rotator", 44, 1, {0: 0.012, 2: -0.012})
    fd = p.add(VRU, "FrameDelay", 58, 1, {0: 3.0})
    l1 = p.add(FUND, "LFO", 65, 1, {0: 0, 2: -3.3})
    l2 = p.add(FUND, "LFO", 74, 1, {0: 0, 2: -3.0})
    va = p.add(FUND, "VCA", 83, 1, {0: 0.5, 1: 0.5})
    d1 = p.add(VRU, "Display", 88, 1)
    p.cable(sh, 0, vm, 0, PURPLE); p.cable(vm, 0, d1, 0, BLUE); p.cable(vm, 0, ro, 0, PURPLE)
    p.cable(ro, 0, fd, 0, PURPLE); p.cable(fd, 0, vm, 1, PURPLE)
    p.cable(l1, 0, va, 2, GREEN); p.cable(va, 0, sh, 4, GREEN)
    p.cable(l2, 0, va, 5, GREEN); p.cable(va, 1, sh, 5, GREEN)
    # Row 2, one oscillator bends another, as patch 8.
    b1 = p.add(VRU, "VideoOsc", 0, 2, {0: OSC1_FREQ, 2: 0.0, 3: 0.0, 4: 6, 5: 0.0, 10: 0, 12: 0.8, 15: 0})
    b2 = p.add(VRU, "VideoOsc", 20, 2, {0: OSC2_FREQ, 1: 0.58, 2: 0.0, 3: 0.0, 4: 6, 10: 1, 12: 1.0, 15: 0, 17: 0.5})
    co = p.add(VRU, "Colorizer", 40, 2)
    d2 = p.add(VRU, "Display", 52, 2)
    p.cable(b1, 0, b2, 10, PURPLE); p.cable(b2, 0, co, 0, PURPLE); p.cable(co, 0, d2, 0, BLUE)
    # Row 3, Scan Processor LIFT IN: the oscillator's picture is drawn, a moving shape on LIFT IN lifts the lines.
    s1 = p.add(VRU, "VideoOsc", 0, 3, {0: 3.0, 1: 0.5, 2: 0.0})
    s2 = p.add(VRU, "Shapes", 20, 3, {0: 5.0, 1: 0.25, 3: 0.35, 13: 1})
    m1 = p.add(FUND, "LFO", 34, 3, {0: 0, 2: -3.0})
    m2 = p.add(FUND, "LFO", 43, 3, {0: 0, 2: -2.6})
    mv = p.add(FUND, "VCA", 52, 3, {0: 0.5, 1: 0.5})
    sp = p.add(VRU, "ScanProcessor", 57, 3, {0: 0.4, 3: 0.1, 4: 0.8, 5: 1, 6: 0.8, 8: 1, 9: 0.5, 10: 0.3, 11: 0.4})
    d3 = p.add(VRU, "Display", 70, 3)
    p.cable(s1, 0, sp, 0, PURPLE); p.cable(s2, 0, sp, 9, PURPLE); p.cable(sp, 0, d3, 0, BLUE)
    p.cable(m1, 0, mv, 2, GREEN); p.cable(mv, 0, s2, 4, GREEN)
    p.cable(m2, 0, mv, 5, GREEN); p.cable(mv, 1, s2, 5, GREEN)
    # Row 4, Feedback fading to black: a small moving dot, gain 0.97, "Fade trails fully to black" on.
    f1 = p.add(VRU, "Shapes", 0, 4, {0: 12.0, 1: 1.0, 3: 0.1, 13: 1})
    n1 = p.add(FUND, "LFO", 14, 4, {0: 0, 2: -2.4})
    n2 = p.add(FUND, "LFO", 23, 4, {0: 0, 2: -2.0})
    nv = p.add(FUND, "VCA", 32, 4, {0: 0.7, 1: 0.7})
    fb = p.add(VRU, "Feedback", 37, 4, {5: 0.97}, data={"fadeTrailsToBlack": True})
    d4 = p.add(VRU, "Display", 49, 4)
    p.cable(f1, 0, fb, 0, PURPLE); p.cable(fb, 0, d4, 0, BLUE)
    p.cable(n1, 0, nv, 2, GREEN); p.cable(nv, 0, f1, 4, GREEN)
    p.cable(n2, 0, nv, 5, GREEN); p.cable(nv, 1, f1, 5, GREEN)
    notes(p, 80, 4, "Everything new in 2.3.0, one row each, top to bottom.\n"
          "1. Picture to CV: a moving picture goes into the Video Probe. A slow LFO moves the probe across it, "
          "and its LUMA output sets the pitch of the bars on the second oscillator, so the bars bunch and spread as the probe moves.\n"
          "2. A loop through a Frame Delay: a shape is cabled round a loop and leaves a spiralling trail.\n"
          "3. One Video Oscillator bends another through FM IN.\n"
          "4. Scan Processor LIFT IN: the oscillator's picture is drawn as lines, and the moving shape on LIFT IN lifts them.\n"
          "5. Feedback with Fade trails fully to black: a small dot leaves a trail that fades out completely.\n"
          "Each row ends in its own Display.")
    p.save("VRU-2.3.0-test.vcv")

# ---------------------------------------------------------------- VRU-2.4.0-clock-test: Scan Sync against a free LFO, side by side
def clock_test_240():
    """Three rows, each Video Oscillator (test bars) -> Deflection Yoke -> Display. Only what drives the Yoke's H coil
    input (X_BEND_INPUT, 1) differs. A: Scan Sync DOWN. B: Fundamental LFO saw, free. C: the same LFO, RESET by Scan Sync TOP.
    Ids: ScanSync LINES param 0, outputs TOP 0, DOWN 1. LFO params OFFSET 0 (1 is 0 to 10 V, as DOWN), FREQ 2 (log2 Hz),
    saw output 2, RESET input 2 (read from the installed Fundamental 2.6.4). Yoke: X_BEND 0, Y_BEND 1, DEPTH 2, LINES 14,
    PERSIST 17, GLOW 18, S_BEND 19."""
    p = Patch("2.4.0")
    LFO_HZ = 60.5     # a little off the 59.94 Hz picture rate: a free LFO beats against it about once every 1.8 s
    lfo_params = {0: 1, 2: math.log2(LFO_HZ)}
    yoke_params = {0: 0.5, 1: 0.0, 2: 1.0, 14: 240.0, 17: 0.0}
    osc_params = {4: 5}   # PATTERN 5, Grid
    sync = p.add(VRU, "ScanSync", 16, 0, {0: 240.0})
    for row in range(3):
        if row == 0:
            drv = sync
        else:
            drv = p.add(FUND, "LFO", 16, row, lfo_params)
        osc = p.add(VRU, "VideoOsc", 27, row, osc_params)
        yk = p.add(VRU, "DeflectionYoke", 56, row, yoke_params)
        dp = p.add(VRU, "Display", 76, row)
        p.cable(osc, 0, yk, 0, PURPLE)
        p.cable(yk, 0, dp, 0, BLUE)
        if row == 0:
            p.cable(sync, 1, yk, 1, YEL)      # DOWN -> H coil in
        else:
            p.cable(drv, 2, yk, 1, YEL)       # LFO saw -> H coil in
        if row == 2:
            p.cable(sync, 0, drv, 2, GREEN)   # TOP -> LFO RESET
    notes(p, 0, 0, "Clock test: three rows, the same test picture and the same Deflection Yoke in each. "
          "Only what drives the Yoke's H coil input is different.\n"
          "A, top row. Scan Sync DOWN into the Yoke. Expect: the picture stands still, sheared the same way every picture.\n"
          "B, middle row. A free-running Fundamental LFO saw at 60.5 Hz into the same input. "
          "Expect: the picture rolls, because 60.5 Hz is not the picture rate (59.94 Hz).\n"
          "C, bottom row. The same LFO, but Scan Sync TOP goes into its RESET input, which restarts the saw at the top of every picture. "
          "Expect: the picture stands still.\n"
          "Scan Sync LINES and the Yoke LINES are both 240.\n"
          "Known: DOWN through the Yoke is unsteady in the top rows.")
    p.save("VRU-2.4.0-clock-test.vcv")

if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "patches"), exist_ok=True)
    lissajous(); oscillators(); tunnel(); slow_scan(); garden(); muybridge(); delay_loop(); bend(); raster_loop(); test_230(); clock_test_240()
