#!/usr/bin/env python3
"""Write five self-contained 1970s-video-art patches (Video Research Unit 2.3.0).

Each patch uses only the plugin's own generators (plus Fundamental LFOs for slow motion, as the demo
patches do) and ends in a Display, so it shows a picture as soon as it is opened. Parameter and port
ids come from src/*.cpp; check the output with scripts/check-patches.py and scripts/check-demo-ranges.py.
Patches are written as Rack writes them: a zstd-compressed tar holding ./patch.json.

Usage: scripts/make-70s-patches.py [output-dir]   (default: the patches folder of this checkout)"""
import io, json, math, os, re, subprocess, sys, tarfile, time

ROOT = os.environ.get("VRU_SRC") or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # VRU_SRC: a checkout with current res/
OUT = os.path.join(ROOT, "patches")
RACK = os.path.expanduser("~/Library/Application Support/Rack2")
VRU, FUND, CORE = "VideoResearchUnit", "Fundamental", "Core"
VER = {VRU: "2.3.0", FUND: "2.6.4", CORE: "2.6.6"}
RED, YEL, GREEN, BLUE, PURPLE = "#f3374b", "#ffb437", "#00b56e", "#3695ef", "#8b4ade"
GAP = 1  # blank HP between modules


def hz_norm(hz, lo=0.01, hi=20.0):       # Dual LFO Chaos rate knob
    return math.log(hz / lo) / math.log(hi / lo)


def yoke_rate(hz):                       # Deflection Yoke coil rate knob (0.1 Hz * 80000^v)
    return math.log(hz / 0.1) / math.log(8000.0 / 0.1)


def lfo_freq(seconds):                   # Fundamental LFO FREQ knob (log2 Hz) for a given period
    return math.log2(1.0 / seconds)


def panel_hp(plugin, model):
    if plugin == CORE:
        return {"Notes": 16, "AudioInterface2": 12}.get(model, 10)
    base = os.path.join(ROOT, "res") if plugin == VRU else os.path.join(RACK, "plugins-mac-arm64", FUND, "res")
    head = open(os.path.join(base, model + ".svg"), errors="ignore").read(4000)
    m = re.search(r'\swidth="([\d.]+)(mm|px)?"', head)
    w = float(m.group(1))
    return int(round(w / 5.08 if m.group(2) == "mm" else w / 15.0))


class Patch:
    """Modules go in rows; x advances by each module's width, so nothing overlaps."""
    def __init__(self):
        self.mods, self.cables, self.cursor = [], [], {}
    def add(self, plugin, model, row, params=None, data=None):
        x = self.cursor.get(row, 0)
        self.cursor[row] = x + panel_hp(plugin, model) + GAP
        m = {"id": len(self.mods) + 1, "plugin": plugin, "model": model, "version": VER[plugin],
             "params": [{"id": k, "value": v} for k, v in sorted((params or {}).items())], "pos": [x, row]}
        if data is not None:
            m["data"] = data
        self.mods.append(m)
        return m["id"]
    def cable(self, out_mod, out_id, in_mod, in_id, color=PURPLE):
        self.cables.append({"id": len(self.cables) + 1, "outputModuleId": out_mod, "outputId": out_id,
                            "inputModuleId": in_mod, "inputId": in_id, "color": color})
    def notes(self, row, text):
        return self.add(CORE, "Notes", row, data={"text": text})
    def save(self, out_dir, name):
        path = os.path.join(out_dir, name)
        if os.path.exists(path):
            raise SystemExit("refusing to overwrite " + path)
        doc = {"version": "2.5.2", "zoom": 1.0, "gridOffset": [0.0, 0.0], "modules": self.mods, "cables": self.cables}
        body = json.dumps(doc, indent=2).encode() + b"\n"
        buf = io.BytesIO()
        now = time.time()
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
            d = tarfile.TarInfo("./"); d.type = tarfile.DIRTYPE; d.mode = 0o755; d.mtime = now; tf.addfile(d)
            f = tarfile.TarInfo("./patch.json"); f.size = len(body); f.mode = 0o644; f.mtime = now; tf.addfile(f, io.BytesIO(body))
            m = tarfile.TarInfo("./modules"); m.type = tarfile.DIRTYPE; m.mode = 0o755; m.mtime = now; tf.addfile(m)
        z = subprocess.run(["/opt/homebrew/bin/zstd", "-c", "-q", "-3"], input=buf.getvalue(), stdout=subprocess.PIPE, check=True)
        with open(path, "wb") as fh:
            fh.write(z.stdout)
        return path


# -------------------------------------------------------------------------------------------------
# 1. Scan relief: the Rutt-Etra idea, Steina and Woody Vasulka's scan processing.
def scan_relief():
    p = Patch()
    # A soft grey dome (a round Shapes picture with a wide grey ramp) and slow rolling rows, mixed in Add.
    sh = p.add(VRU, "Shapes", 0, {0: 8, 1: 1.0, 3: 0.4, 12: 0.7, 13: 0})
    vo = p.add(VRU, "VideoOsc", 0, {0: math.log2(2 * 5.0), 2: 0.0, 3: 0.0, 4: 6, 10: 0, 12: 0.9, 15: 0})
    mx = p.add(VRU, "BusMixer", 0, {0: 0.3, 1: 0})
    sp = p.add(VRU, "ScanProcessor", 0, {0: 0.3, 3: 0.0, 4: 1.0, 5: 2, 6: 0.5, 8: 0, 9: 0.3, 10: 0.35, 11: 0.15})
    fo = p.add(VRU, "Focus", 0, {0: 0.1})
    dp = p.add(VRU, "Display", 0)
    lf = p.add(VRU, "DualLfoChaos", 1, {0: hz_norm(0.03), 1: 0.5, 2: hz_norm(0.021), 3: 0.5, 4: 0.2, 5: 0.5})
    l1 = p.add(FUND, "LFO", 1, {0: 0, 2: lfo_freq(41)})
    av = p.add(FUND, "8vert", 1, {0: 0.04})
    p.notes(1, "Raster relief, after the Rutt-Etra scan processor that Steina and Woody Vasulka used in the early 1970s: "
            "every scan line is lifted by the brightness of the picture under it, so a soft dome and rolling bars "
            "stand up out of a flat screen as a green phosphor landscape.\n"
            "The picture under it is a round Shapes dome added to the rows of a Video Oscillator. "
            "Very slow chaos and one slow LFO drift the dome and tilt the lift.\n"
            "Try LUMA GAIN on the Scan Processor first: it sets how high the picture stands. Then FREQ A on the Video Oscillator "
            "(how many ridges), INTENSITY, and PERSIST for a ghost trail.")
    p.cable(sh, 0, mx, 0); p.cable(vo, 0, mx, 1)
    p.cable(mx, 0, sp, 0); p.cable(sp, 0, fo, 0); p.cable(fo, 0, dp, 0, BLUE)
    p.cable(lf, 2, sh, 4, GREEN)         # chaos X -> dome X
    p.cable(lf, 3, sh, 5, GREEN)         # chaos Y -> dome Y
    p.cable(l1, 0, av, 0, YEL); p.cable(av, 0, sp, 1, YEL)   # slow sine x 0.04 -> luma gain
    return p


# -------------------------------------------------------------------------------------------------
# 2. Breathing feedback: the camera-at-the-monitor loop (Skip Sweeney, Vasulka's early feedback studies).
def feedback_bloom():
    p = Patch()
    sh = p.add(VRU, "Shapes", 0, {0: 5, 1: 0.3, 2: 0.35, 3: 0.2, 13: 1})
    vm = p.add(VRU, "VideoMixer", 0, {0: 0.02, 4: 1, 8: 1.0, 25: 0.8, 29: 1})
    fb = p.add(VRU, "Feedback", 0, {0: -0.06, 1: 0.01, 4: 0.05, 5: 0.94, 9: 0})
    ro = p.add(VRU, "Rotator", 0, {0: 0.004, 2: -0.01, 7: 2})
    fd = p.add(VRU, "FrameDelay", 0, {0: 2.0})
    dp = p.add(VRU, "Display", 0)
    lf = p.add(VRU, "DualLfoChaos", 1, {0: hz_norm(0.045), 1: 0.5, 2: hz_norm(0.027), 3: 0.5, 4: 0.2, 5: 0.5})
    l1 = p.add(FUND, "LFO", 1, {0: 0, 2: lfo_freq(33)})
    av = p.add(FUND, "8vert", 1, {0: 0.3})
    p.notes(1, "Video feedback, the way Skip Sweeney and the early Vasulka studies made it: a camera pointed at its own monitor, "
            "here a loop of picture, turn, shrink and colour shift. One small shape is the only thing fed in; "
            "everything else grows out of the loop.\n"
            "The Video Mixer tints the shape. The Feedback module keeps every earlier frame, shrinks, turns and shifts its colour a little on each pass, "
            "so the shape leaves a rainbow spiral; the Rotator turns the whole picture slowly and the Frame Delay holds it a couple of frames back, which gives it its drift.\n"
            "Slow chaos nudges the shape; a slow LFO sways the turn so the tunnel breathes.\n"
            "Try GAIN on the Feedback first (higher holds the picture longer), then ROTATE, ZOOM and HUE. "
            "Press CLEAR after a big change.")
    p.cable(sh, 0, vm, 0); p.cable(vm, 0, fb, 0); p.cable(fb, 0, ro, 0)
    p.cable(ro, 0, fd, 0); p.cable(fd, 0, dp, 0, BLUE)
    p.cable(lf, 2, sh, 4, GREEN); p.cable(lf, 3, sh, 5, GREEN)
    p.cable(l1, 0, av, 0, YEL); p.cable(av, 0, fb, 2, YEL)    # slow sine x 0.3 -> Feedback rotate
    return p


# -------------------------------------------------------------------------------------------------
# 2b. The same idea filling the whole screen: tunnels and petals that cycle through colour.
def feedback_bloom2():
    p = Patch()
    sh = p.add(VRU, "Shapes", 0, {0: 6, 1: 0.4, 3: 0.4, 4: 0.12, 8: 1, 13: 1})
    ra = p.add(VRU, "Ramps", 0, {0: 2, 1: 0.5, 4: 3, 5: 0.3, 8: 3})
    vm = p.add(VRU, "VideoMixer", 0, {0: 0.02, 1: 0.55, 4: 1, 5: 1, 8: 1.0, 25: 0.6, 26: 0.2, 29: 1})
    fb = p.add(VRU, "Feedback", 0, {0: 0.04, 1: 0.02, 4: 0.06, 5: 0.9, 8: 1, 9: 0})
    ro = p.add(VRU, "Rotator", 0, {0: 0.0, 1: 0.02, 7: 3, 8: 6})
    fd = p.add(VRU, "FrameDelay", 0, {0: 2.0})
    cl = p.add(VRU, "ColorLab", 0, {1: 1.5, 3: 1.6})
    dp = p.add(VRU, "Display", 0)
    lf = p.add(VRU, "DualLfoChaos", 1, {0: hz_norm(0.05), 1: 0.15, 2: hz_norm(0.031), 3: 0.15, 4: 0.2, 5: 0.5})
    p.notes(1, "Video feedback filling the screen, as when a camera looks at its own monitor (Skip Sweeney, the early Vasulka studies): "
            "a tunnel of turning, widening rings and petals whose colour changes a little on every trip round the loop.\n"
            "A drifting shape and a faint set of rings are tinted and fed in at low level. The Feedback module expands, turns and shifts the colour of the "
            "whole picture on every pass and wraps what leaves the edge back in, so the picture never settles. The Rotator folds it into a six-fold mandala "
            "the Frame Delay holds it two frames back and a Color Lab adds contrast and colour. Slow chaos breathes the zoom and the turn.\n"
            "Try GAIN on the Feedback first (lower and it fades, higher and it floods), then ZOOM, ROTATE and HUE. Press CLEAR after a big change.")
    p.cable(sh, 0, vm, 0); p.cable(ra, 0, vm, 1)
    p.cable(vm, 0, fb, 0); p.cable(fb, 0, ro, 0); p.cable(ro, 0, fd, 0); p.cable(fd, 0, cl, 0); p.cable(cl, 0, dp, 0, BLUE)
    p.cable(lf, 2, fb, 1, GREEN); p.cable(lf, 3, fb, 2, GREEN)
    return p


# -------------------------------------------------------------------------------------------------
# 3. Colorized forms keyed over one another: Sandin Image Processor and Paik-Abe synthesizer.
def colorized_keying():
    p = Patch()
    ra = p.add(VRU, "Ramps", 0, {0: 2, 1: 0.55, 4: 0, 5: 0.9, 8: 3})                 # radial rings x sine bands
    ws = p.add(VRU, "WaveShaper", 0, {0: 1.4, 2: 0.55, 3: 0.35})
    c1 = p.add(VRU, "Colorizer", 0, {0: 0.0, 1: 0.3, 2: 0.6, 3: 0.85, 4: 1.0, 5: 1.0, 6: 1.0, 7: 1.0, 8: 1.0, 9: 0})
    vo = p.add(VRU, "VideoOsc", 0, {0: math.log2(2 * 6.0), 2: 0.0, 3: 0.0, 4: 6, 10: 1, 12: 0.9, 15: 0})
    cl = p.add(VRU, "ColorLab", 0, {0: 0.35, 1: 1.6, 4: 0.5})
    sh = p.add(VRU, "Shapes", 1, {0: 6, 1: 0.2, 3: 0.45, 8: 1, 9: 5, 10: 0.5, 13: 1})
    vm = p.add(VRU, "VideoMixer", 1, {17: 1, 18: 0.5, 19: 0.06, 22: 1})
    c2 = p.add(VRU, "ColorLab", 1, {1: 1.3, 5: 0.15})
    dp = p.add(VRU, "Display", 1)
    lf = p.add(VRU, "DualLfoChaos", 2, {0: hz_norm(0.05), 1: 0.5, 2: hz_norm(0.031), 3: 0.5, 4: 0.2, 5: 0.5})
    p.notes(2, "Colorized forms keyed over one another, in the manner of the Sandin Image Processor and the Paik-Abe video synthesizer.\n"
            "Rings and rays from the Ramps are folded by the Wave Shaper and painted by the Colorizer. "
            "Bars from a Video Oscillator go through Color Lab. Neither is the picture you see: they sit in the Video Mixer, "
            "and a drifting polygon (the matte) cuts a hole in the rings and shows the bars through it, with a hard edge, the way a keyer does.\n"
            "Try THRESH on the Video Mixer key first (how much of the shape cuts through), then BANDS on the Colorizer and HUE on either Color Lab.")
    p.cable(ra, 0, ws, 0); p.cable(ws, 0, c1, 0); p.cable(c1, 0, vm, 0)   # A: colorized rings
    p.cable(vo, 0, cl, 0); p.cable(cl, 0, vm, 5)                          # KEY picture: coloured bars
    p.cable(sh, 0, vm, 6)                                                 # MATTE: polygon
    p.cable(vm, 0, c2, 0); p.cable(c2, 0, dp, 0, BLUE)
    p.cable(lf, 2, sh, 4, GREEN); p.cable(lf, 3, sh, 5, GREEN)
    return p


# -------------------------------------------------------------------------------------------------
# 4. Direct video: oscillators and shapes only, no camera (Stephen Beck, Direct Video Synthesizer).
def direct_video():
    p = Patch()
    ra = p.add(VRU, "Ramps", 0, {0: 2, 1: 0.6, 4: 3, 5: 0.3, 8: 3})
    vo = p.add(VRU, "VideoOsc", 0, {0: math.log2(2 * 9.0), 1: 0.3, 2: 0.0, 3: 0.0, 4: 6, 10: 1, 12: 0.85, 15: 0})
    sh = p.add(VRU, "Shapes", 0, {0: 4, 1: 0.1, 3: 0.4, 8: 1, 9: 6, 10: 0.55, 13: 1})
    vm = p.add(VRU, "VideoMixer", 0, {0: 0.0, 1: 0.33, 2: 0.66, 4: 1, 5: 1, 6: 1, 8: 1.0, 25: 1.0, 26: 1.0, 27: 1.0, 29: 1})
    dp = p.add(VRU, "Display", 0)
    lf = p.add(VRU, "DualLfoChaos", 1, {0: hz_norm(0.06), 1: 0.5, 2: hz_norm(0.04), 3: 0.5, 4: 0.25, 5: 0.5})
    p.notes(1, "Direct video: no camera anywhere, as in Stephen Beck's Direct Video Synthesizer (1970-73). "
            "Three generators each draw a picture from nothing: rings and rays, rows of bars, and a turning ring of shapes.\n"
            "The Video Mixer is set to ALL and tints each one a primary, red, green and blue, so where they overlap the colours add into new ones. "
            "Slow chaos moves the shapes across the screen.\n"
            "Try the LEVEL knobs on the Video Mixer first, one per generator, then FREQ A on the oscillator and the Ramps frequencies.")
    p.cable(ra, 0, vm, 0); p.cable(vo, 0, vm, 1); p.cable(sh, 0, vm, 2)
    p.cable(vm, 0, dp, 0, BLUE)
    p.cable(lf, 2, sh, 4, GREEN); p.cable(lf, 3, sh, 5, GREEN)
    return p


# -------------------------------------------------------------------------------------------------
# 5. Wobbulator: Nam June Paik's magnets on a broadcast picture.
def wobbulator():
    p = Patch()
    vo = p.add(VRU, "VideoOsc", 0, {4: 1, 5: 1.0})                       # pattern 1: SMPTE colour bars
    dy = p.add(VRU, "DeflectionYoke", 0, {0: 0.8, 1: 0.35, 2: 0.7, 3: yoke_rate(120.6), 9: yoke_rate(61.7),
                                           19: 0.5, 20: yoke_rate(91.3), 17: 0.35, 18: 0.4})
    dp = p.add(VRU, "Display", 0)
    lf = p.add(VRU, "DualLfoChaos", 1, {0: hz_norm(0.07), 1: 0.5, 2: hz_norm(0.05), 3: 0.5, 4: 0.2, 5: 0.5})
    p.notes(1, "Wobbulator, after Nam June Paik's Magnet TV (1965) and the Wobbulator he built with Shuya Abe: "
            "a plain broadcast colour-bar picture is bent by deflection coils, the way a magnet held beside a picture tube bends the raster.\n"
            "Three coils bend the picture, one sideways, one up and down and one in a swirl, each at its own slow rate. "
            "Slow chaos sways how hard they pull, so the bars melt, ripple and wind into a vortex and let go.\n"
            "Try DEPTH on the Deflection Yoke first, then the H, V and S coil amounts and their rates.")
    p.cable(vo, 0, dy, 0); p.cable(dy, 0, dp, 0, BLUE)
    p.cable(lf, 2, dy, 3, GREEN)
    return p


PATCHES = [("70s scan relief.vcv", scan_relief), ("70s feedback bloom.vcv", feedback_bloom), ("70s feedback bloom 2.vcv", feedback_bloom2),
           ("70s colorized keying.vcv", colorized_keying), ("70s direct video.vcv", direct_video),
           ("70s wobbulator.vcv", wobbulator)]

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    os.makedirs(out, exist_ok=True)
    for name, fn in PATCHES:
        print(fn().save(out, name))
