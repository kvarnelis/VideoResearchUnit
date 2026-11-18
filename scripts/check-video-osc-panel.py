#!/usr/bin/env python3
"""Checks the Video Oscillator panel layout in src/video_osc_layout.hpp: every control at its true footprint
(black knob 9.6 mm, small knob 7.68, jack 8.03, switch 4.74 x 6.99, light 3, output plate 11.8 high), every label
and switch position word at its true text size, measured in Nunito Bold. Fails if any two boxes come within 0.5 mm,
or if a box leaves the panel (1.5 mm margin). With --png FILE it also draws the panel to scale (needs ImageMagick).
Usage: scripts/check-video-osc-panel.py [--png docs/video-osc-panel-layout.png]"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = "/Applications/VCV Rack 2 Pro.app/Contents/Resources/res/fonts/Nunito-Bold.ttf"
GAP, MARGIN = 0.5, 1.5
CAP = 0.561          # capital height / NanoVG size (panel_style.hpp)
EM = 1.255           # NanoVG size / em
LABEL, NOTE = 3.8, 3.2
R = {"KNOB": 4.8, "SNAP": 4.8, "SMALL": 3.84, "SWITCH": 3.5, "JACK": 4.0, "LIGHT": 4.0}   # label offset radius
GAP_LABEL = 1.4

src = open(os.path.join(ROOT, "src", "video_osc_layout.hpp")).read()
W = float(re.search(r"kVideoOscPanelWidth\s*=\s*([\d.]+)f", src).group(1))
H = 128.5
items = [(m[0], float(m[1]), float(m[2]), m[3], m[4], float(m[5])) for m in re.findall(
    r'X\((\w+),\s*([\d.]+)f,\s*([\d.]+)f,\s*"([^"]*)",\s*([\w:]+),\s*([\d.]+)f?\)', src)]
if not items:
    sys.exit("no layout items found")

widths = {}
def text_width(text, size):
    """Width in mm of `text` at NanoVG size `size`, from Nunito Bold."""
    if text not in widths:
        out = subprocess.run(["magick", "-font", FONT, "-pointsize", "100", "label:" + text, "-format", "%w", "info:"],
                             capture_output=True, text=True)
        widths[text] = float(out.stdout) / 100.0
    return widths[text] * size / EM

boxes = []   # (name, x0, y0, x1, y1, kind, text)
def add(name, cx, cy, w, h, kind, text=""):
    boxes.append((name, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, kind, text))
def add_text(name, cx, cy, text, size, align="c"):
    w, h = text_width(text, size), CAP * size + 0.3
    x0 = cx - w / 2 if align == "c" else cx
    boxes.append((name, x0, cy - h / 2, x0 + w, cy + h / 2, "text", text))
def label_above(name, x, y, text, radius):
    if text:
        add_text(name + " label", x, y - radius - GAP_LABEL - CAP * LABEL / 2, text, LABEL)

add("preview", 4 + 27, 19 + 18, 54, 36, "preview")
add("panel header", W / 2, 8.5, 60, 14, "header")
for kind, x, y, text, ident, extra in items:
    name = f"{kind} {text or ident} @({x},{y})"
    if kind in ("KNOB", "SNAP"):
        add(name, x, y, 9.6, 9.6, "knob"); label_above(name, x, y, text, R[kind])
    elif kind == "SMALL":
        add(name, x, y, 7.68, 7.68, "knob"); label_above(name, x, y, text, R[kind])
    elif kind == "SWITCH":
        add(name, x, y, 4.74, 6.99, "switch")
        add_text(name + " BARS", x, y - 5.4, "BARS", NOTE)
        add_text(name + " ROWS", x, y + 5.4, "ROWS", NOTE)
        if text:
            add_text(name + " label", x, y - 8.4, text, LABEL)
    elif kind in ("JACK", "CELL"):
        add(name, x, y, 8.03, 8.03, "jack"); label_above(name, x, y, text, R["JACK"])
    elif kind == "LIGHT":
        add(name, x, y, 3, 3, "jack"); label_above(name, x, y, text, R["JACK"])
    elif kind == "OUT":
        boxes.append((name, x - extra / 2, y - 4.2, x + extra / 2, y + 7.6, "plate", text))
    elif kind == "TEXT":
        add_text(name, x, y, text, extra)
    elif kind == "LEFT":
        add_text(name, x, y, text, extra, "l")
    elif kind == "RULE":
        boxes.append((name, x, y - 0.1, extra, y + 0.1, "rule", ""))

fails = []
for name, x0, y0, x1, y1, kind, text in boxes:
    if kind == "plate":
        need = text_width(text, 3.4) + 2 * 0.5   # plate labels are drawn at 3.4 mm
        if need > (x1 - x0):
            fails.append(f"plate label does not fit with 0.5 mm each side ({need:.2f} mm needed, plate {x1 - x0:.2f}): {name}")
for name, x0, y0, x1, y1, kind, text in boxes:
    if x0 < MARGIN or y0 < 0 or x1 > W - MARGIN or y1 > H - MARGIN:
        if kind not in ("header",):
            fails.append(f"outside the panel (margin {MARGIN}): {name} [{x0:.1f},{y0:.1f},{x1:.1f},{y1:.1f}]")
for i in range(len(boxes)):
    for j in range(i + 1, len(boxes)):
        a, b = boxes[i], boxes[j]
        if a[5] == "header" or b[5] == "header":
            if a[5] == b[5]:
                continue
            other = b if a[5] == "header" else a
            if other[2] < 15.4 + 0.5:
                fails.append(f"in the header band: {other[0]}")
            continue
        dx = max(a[1] - b[3], b[1] - a[3])
        dy = max(a[2] - b[4], b[2] - a[4])
        if max(dx, dy) < GAP:
            fails.append(f"too close ({max(dx, dy):.2f} mm): {a[0]}  <>  {b[0]}")

if "--png" in sys.argv:
    out = sys.argv[sys.argv.index("--png") + 1]
    S = 10  # px per mm
    cmd = ["magick", "-size", f"{int(W*S)}x{int(H*S)}", "xc:#e8e8e6", "-font", FONT]
    colours = {"knob": "#2a2a2a", "switch": "#6a6a6a", "jack": "#3a6ea5", "plate": "#111111", "preview": "#555555", "rule": "#1b1b1b"}
    cmd += ["-stroke", "#1b1b1b", "-strokewidth", "1.5", "-draw", f"line {4*S},{15.4*S} {(W-4)*S},{15.4*S}"]
    for name, x0, y0, x1, y1, kind, text in boxes:
        if kind == "header":
            cmd += ["-stroke", "none", "-fill", "#1b1b1b", "-pointsize", "34", "-gravity", "NorthWest",
                    "-annotate", f"+{(W/2-25)*S}+{4*S}", "VIDEO OSCILLATOR"]
        elif kind == "text":
            size = (y1 - y0 - 0.3) / CAP
            cmd += ["-stroke", "none", "-fill", "#1b1b1b", "-pointsize", f"{size/EM*S:.1f}", "-gravity", "NorthWest",
                    "-annotate", f"+{x0*S:.0f}+{(y0 - (size - (y1-y0))/2.2)*S:.0f}", text]
        elif kind == "rule":
            cmd += ["-stroke", "#1b1b1b", "-strokewidth", "2", "-draw", f"line {x0*S},{y0*S} {x1*S},{y0*S}"]
        elif kind in ("knob",):
            cmd += ["-stroke", "none", "-fill", colours[kind], "-draw", f"circle {(x0+x1)/2*S},{(y0+y1)/2*S} {(x0+x1)/2*S},{y0*S}"]
        elif kind == "jack":
            cmd += ["-stroke", "none", "-fill", colours[kind], "-draw", f"circle {(x0+x1)/2*S},{(y0+y1)/2*S} {(x0+x1)/2*S},{y0*S}"]
        elif kind == "preview":
            cmd += ["-stroke", "none", "-fill", "#555555", "-draw", f"rectangle {x0*S},{y0*S} {x1*S},{y1*S}"]
        else:
            cmd += ["-stroke", "none", "-fill", colours[kind], "-draw", f"roundrectangle {x0*S},{y0*S} {x1*S},{y1*S} 6,6"]
            if kind == "plate":
                cmd += ["-fill", "#f2f2f0", "-pointsize", f"{3.4/EM*S:.1f}", "-gravity", "NorthWest", "-annotate",
                        f"+{((x0+x1)/2 - text_width(text, 3.4)/2)*S:.0f}+{(y0+4.2+4.9)*S:.0f}", text]
    cmd += ["-stroke", "#1b1b1b", "-strokewidth", "2", "-fill", "none", "-draw", f"rectangle 1,1 {W*S-2},{H*S-2}", out]
    subprocess.run(cmd, check=True)

print(f"video osc panel: {len(boxes)} boxes, {W:.2f} mm wide ({W/5.08:.1f} HP), {len(fails)} problems")
for f in fails:
    print("  " + f)
sys.exit(1 if fails else 0)
