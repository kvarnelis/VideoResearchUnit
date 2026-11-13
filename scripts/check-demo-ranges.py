#!/usr/bin/env python3
"""Range audit for the demo patches: for every cable into a VRU CV input, work out the range the
destination really sweeps, from the knob in the patch, the source (Fundamental LFO, Dual LFO Chaos,
Octoscillator, through any Fundamental VCA or 8vert on the way), the module's volts-to-knob scale
(from src/*.cpp) and the attenuverter, clamped to the knob range. Flags a destination that would push
the picture off-screen, dim it to nothing, or swing it too hard.

Usage: scripts/check-demo-ranges.py [--quiet] [patch.vcv ...]   (default: patches/[1-9]*.vcv)
Exit status 1 when anything is flagged. The scales below are copied from the module sources:
ScanProcessor.cpp, VideoOsc.cpp, Colorizer.cpp, Feedback.cpp, Rotator.cpp, shapes_math.hpp, ScopeTrace.cpp."""
import glob, json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIG = 1e9
# (model, input id) -> (name, knob param id, volts->knob per volt, lo, hi, attenuverter param id or None, kind, limit)
# kind: "pos"  flag when the value strays more than `limit` from the knob (screen position)
#       "lift" luma gain: flag when it leaves limit = (lo, hi)
#       "floor" flag when the value falls below `limit` (a level, size or intensity that must not go dark)
#       "ceil" flag when the value rises above `limit`
#       "free" no flag (hue, wrapped angles, colour, small detunes)
CV = {
    ("ScanProcessor", 1): ("LUMA GAIN", 0, 0.2, -1, 1, None, "lift", (0.1, 0.5)),
    ("ScanProcessor", 5): ("INTENSITY", 6, 0.3, 0, 3, None, "floor", 0.6),
    ("ScanProcessor", 6): ("H POS", 1, 0.2, -1, 1, None, "pos", 0.3),
    ("ScanProcessor", 7): ("V POS", 3, 0.2, -1, 1, None, "pos", 0.3),
    ("VideoOsc", 0): ("FREQ A", 0, 0.4, 0.05, 24, None, "free", None),
    ("VideoOsc", 1): ("HUE", 1, 0.1, 0, 1, None, "free", None),
    ("Colorizer", 7): ("OFFSET", 10, 0.1, -BIG, BIG, None, "free", None),
    ("Feedback", 1): ("ZOOM", 0, 0.1, -1, 1, None, "free", None),
    ("Feedback", 2): ("ROTATE", 1, 0.1, -0.5, 0.5, None, "free", None),
    ("Rotator", 1): ("ANGLE", 0, 0.1, -BIG, BIG, None, "free", None),
    ("Rotator", 3): ("ZOOM", 2, 0.1, -1, 1, None, "free", None),
    ("Shapes", 0): ("SIDES", 0, 0.9, 3, 12, None, "free", None),
    ("Shapes", 2): ("POINT", 2, 0.1, 0, 1, None, "free", None),
    ("Shapes", 3): ("SIZE", 3, 0.1, 0, 1, None, "floor", 0.2),
    ("Shapes", 7): ("TURN", 7, 0.1, -BIG, BIG, None, "free", None),
    ("Shapes", 8): ("MELT", 11, 0.1, 0, 1, None, "free", None),
    ("ScopeTrace", 8): ("ROTATION", 10, 0.2, -BIG, BIG, 14, "free", None),
    ("ScopeTrace", 12): ("DWELL", 21, 0.1, 0, 1, 22, "free", None),
}
ATT_DEFAULT = 1.0
# default knob values for params a patch does not list
DEFAULT = {("ScanProcessor", 0): 0.2, ("ScanProcessor", 1): 0, ("ScanProcessor", 3): 0, ("ScanProcessor", 6): 0.6,
           ("VideoOsc", 0): 4.0, ("VideoOsc", 1): 0.58, ("Colorizer", 10): 0, ("Feedback", 0): 0,
           ("Feedback", 1): 0, ("Rotator", 0): 0, ("Rotator", 2): 0, ("Shapes", 0): 4, ("Shapes", 2): 0,
           ("Shapes", 3): 0.45, ("Shapes", 7): 0, ("Shapes", 11): 0, ("ScopeTrace", 10): 0,
           ("ScopeTrace", 21): 0.5}

def param(m, pid, default=None):
    for p in m.get("params", []):
        if p["id"] == pid:
            return p["value"]
    return DEFAULT.get((m["model"], pid), default)

def source_range(mods, cables, mod_id, out_id, depth=0):
    """(lo, hi, description) of the voltage on an output, following Fundamental VCA and 8vert."""
    m = mods[mod_id]
    model = m["model"]
    if model == "LFO":
        unipolar = param(m, 0, 1.0) > 0.0
        lo, hi = (0.0, 10.0) if unipolar else (-5.0, 5.0)
        return lo, hi, "LFO " + ("0..10 V" if unipolar else "+-5 V")
    if model == "DualLfoChaos":
        depth_ = {0: param(m, 1, 1.0), 1: param(m, 3, 1.0)}.get(out_id, 1.0)
        bip = param(m, 6, 1.0) > 0.5
        a = 5.0 * depth_
        return (-a, a, f"Dual LFO Chaos +-{a:g} V") if bip else (5 - a, 5 + a, f"Dual LFO Chaos {5-a:g}..{5+a:g} V")
    if model == "Octoscillator":
        osc = out_id // 3
        d = param(m, 13 + osc, 0.5) * param(m, 3, 1.0)
        rng = int(param(m, 4, 0))
        base = {0: (-5.0, 5.0), 1: (0.0, 5.0), 2: (-1.0, 1.0)}[rng]
        off = param(m, 21 + osc, 0.0)
        # depth scales the swing about the centre of the range's own span, as octoscillator::voltage does
        lo = (base[0] * d if rng != 1 else 2.5 - 2.5 * d) + off
        hi = (base[1] * d if rng != 1 else 2.5 + 2.5 * d) + off
        return lo, hi, f"Octoscillator {lo:+.2f}..{hi:+.2f} V"
    if model in ("VCA", "8vert"):
        if model == "VCA":
            in_port, level = {0: (2, 0), 1: (5, 1)}[out_id]
            gain = param(m, level, 1.0)
        else:
            in_port, gain = out_id, param(m, out_id, 0.0)
        src = next((c for c in cables if c["inputModuleId"] == mod_id and c["inputId"] == in_port), None)
        if src is None:
            return 0.0, 0.0, model + " (nothing patched in)"
        lo, hi, d = source_range(mods, cables, src["outputModuleId"], src["outputId"], depth + 1)
        a, b = sorted((lo * gain, hi * gain))
        return a, b, f"{d} x {gain:g} ({model})"
    return -5.0, 5.0, model + " (assumed +-5 V)"

def audit(path):
    doc = json.load(open(path))
    mods = {m["id"]: m for m in doc["modules"]}
    cables = doc["cables"]
    rows, flags = [], []
    for c in cables:
        dst = mods[c["inputModuleId"]]
        key = (dst["model"], c["inputId"])
        if dst["plugin"] != "VideoResearchUnit" or key not in CV:
            continue
        name, pid, per_volt, lo_k, hi_k, att_id, kind, limit = CV[key]
        lo_v, hi_v, desc = source_range(mods, cables, c["outputModuleId"], c["outputId"])
        att = param(dst, att_id, ATT_DEFAULT) if att_id is not None else 1.0
        knob = param(dst, pid, 0.0)
        a, b = sorted((knob + att * lo_v * per_volt, knob + att * hi_v * per_volt))
        a, b = max(a, lo_k), min(b, hi_k)
        note = ""
        if kind == "pos" and max(abs(a), abs(b)) > limit:
            note = f"off-screen drift beyond +-{limit}"
        elif kind == "lift" and (b > limit[1] or a < limit[0]):
            note = f"luma gain outside {limit[0]}..{limit[1]}"
        elif kind == "floor" and a < limit:
            note = f"falls below {limit}"
        elif kind == "ceil" and b > limit:
            note = f"rises above {limit}"
        rows.append((dst["model"], name, knob, a, b, desc, note))
        if note:
            flags.append((dst["model"], name, note))
    # bus sanity: every bus input must come from a bus output
    bus_out = {"VideoOsc": [0], "BusMixer": [0], "Colorizer": [0], "Shapes": [0], "Rotator": [0], "Feedback": [0],
               "DeflectionYoke": [0], "ScanProcessor": [0], "ScopeTrace": [0], "VideoPlayer": [0], "Octoscillator": []}
    bus_in = {"BusMixer": [0, 1, 2], "Colorizer": [0], "Feedback": [0], "Rotator": [0], "DeflectionYoke": [0],
              "ScanProcessor": [0], "Display": [0]}
    for c in cables:
        src, dst = mods[c["outputModuleId"]], mods[c["inputModuleId"]]
        if dst["model"] in bus_in and c["inputId"] in bus_in[dst["model"]]:
            if src["plugin"] != "VideoResearchUnit" or c["outputId"] not in bus_out.get(src["model"], []):
                flags.append((dst["model"], "bus input", f"fed by {src['model']} output {c['outputId']}, not a video bus"))
    displays = [m for m in doc["modules"] if m["model"] == "Display"]
    for d in displays:
        if not any(c["inputModuleId"] == d["id"] and c["inputId"] == 0 for c in cables):
            flags.append(("Display", "bus input", "nothing patched in"))
    return rows, flags

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    quiet = "--quiet" in sys.argv
    files = args or sorted(glob.glob(os.path.join(ROOT, "patches", "[1-9]*.vcv")))
    bad = 0
    for f in files:
        rows, flags = audit(f)
        print(os.path.basename(f))
        if not quiet:
            for model, name, knob, a, b, desc, note in rows:
                print(f"  {model:11} {name:10} knob {knob:6.3f}  sweeps {a:7.3f} .. {b:7.3f}   <- {desc}" + (f"   ** {note}" if note else ""))
        for model, name, note in flags:
            print(f"  FLAG {model} {name}: {note}") if quiet else None
        bad += bool(flags)
    sys.exit(1 if bad else 0)

main()
