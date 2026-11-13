#!/usr/bin/env python3
"""Check the demo patches in patches/ without Rack.

For each patch: it is valid JSON; every module's plugin and model exist (VRU from plugin.json,
Fundamental from the installed plugin, Core from Rack's Core.json); every param id is in range;
every cable names existing modules and valid port ids, and no input has two cables;
module boxes (position in HP, width from the panel SVG) do not overlap.

Usage: scripts/check-patches.py [patch.vcv ...]   (default: patches/[1-9]*.vcv)
Port counts for VRU modules are taken from the enums in src/*.cpp; Fundamental and Core
counts from the Fundamental 2.6.4 sources and Rack's Core. A model missing from the table is
reported as a warning, and its ports and params are not range-checked."""
import glob, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RACK = os.path.expanduser("~/Library/Application Support/Rack2")
APP = "/Applications/VCV Rack 2 Pro.app/Contents/Resources"
# model: (params, inputs, outputs)
COUNTS = {
    ("VideoResearchUnit", "ScopeTrace"): (28, 18, 3), ("VideoResearchUnit", "VideoOsc"): (17, 6, 1),
    ("VideoResearchUnit", "BusMixer"): (4, 5, 1), ("VideoResearchUnit", "Colorizer"): (11, 8, 1),
    ("VideoResearchUnit", "Display"): (3, 5, 1), ("VideoResearchUnit", "ScanProcessor"): (12, 9, 2),
    ("VideoResearchUnit", "Shapes"): (14, 10, 1), ("VideoResearchUnit", "Rotator"): (11, 8, 1),
    ("VideoResearchUnit", "Octoscillator"): (29, 3, 24), ("VideoResearchUnit", "Feedback"): (10, 9, 1),
    ("VideoResearchUnit", "DeflectionYoke"): (28, 29, 1), ("VideoResearchUnit", "DualLfoChaos"): (7, 4, 6),
    ("VideoResearchUnit", "VideoPlayer"): (5, 4, 2),
    ("Fundamental", "VCO"): (8, 4, 4), ("Fundamental", "LFO"): (7, 5, 4), ("Fundamental", "VCA"): (2, 6, 2), ("Fundamental", "8vert"): (8, 8, 8),
    ("Core", "AudioInterface2"): (0, 2, 2), ("Core", "Notes"): (0, 0, 0),
}
CORE_SVG = {"AudioInterface2": "Audio2.svg", "Notes": "Notes.svg"}

def svg_hp(path):
    head = open(path, errors="ignore").read(4000)
    m = re.search(r'\swidth="([\d.]+)(mm|px)?"', head)
    if not m:
        return None
    w = float(m.group(1))
    return w / 5.08 if m.group(2) == "mm" else w / 15.0

def load_models():
    out = {}
    vru = json.load(open(os.path.join(ROOT, "plugin.json")))
    out["VideoResearchUnit"] = {m["slug"] for m in vru["modules"]}
    fj = os.path.join(RACK, "plugins-mac-arm64/Fundamental/plugin.json")
    out["Fundamental"] = {m["slug"] for m in json.load(open(fj))["modules"]}
    out["Core"] = {m["slug"] for m in json.load(open(os.path.join(APP, "Core.json")))["modules"]}
    return out

def panel_path(plugin, model):
    if plugin == "VideoResearchUnit":
        return os.path.join(ROOT, "res", model + ".svg")
    if plugin == "Fundamental":
        return os.path.join(RACK, "plugins-mac-arm64/Fundamental/res", model + ".svg")
    return os.path.join(APP, "res/Core", CORE_SVG.get(model, model + ".svg"))

def check(path, models):
    errs, warns = [], []
    try:
        d = json.load(open(path))
    except Exception as e:
        return [f"not valid JSON: {e}"], []
    for k in ("version", "modules", "cables"):
        if k not in d:
            errs.append(f"missing top-level key {k}")
    mods = {}
    boxes = []
    for m in d.get("modules", []):
        mid = m.get("id")
        if mid in mods:
            errs.append(f"duplicate module id {mid}")
        mods[mid] = m
        pl, mo = m.get("plugin"), m.get("model")
        if pl not in models or mo not in models[pl]:
            errs.append(f"module {mid}: unknown {pl}/{mo}")
            continue
        c = COUNTS.get((pl, mo))
        if c is None:
            warns.append(f"module {mid} {pl}/{mo}: no port counts in checker table")
        else:
            for p in m.get("params", []):
                if not 0 <= p["id"] < c[0]:
                    errs.append(f"module {mid} {mo}: param id {p['id']} out of range (0..{c[0]-1})")
        sp = panel_path(pl, mo)
        hp = svg_hp(sp) if os.path.exists(sp) else None
        if hp is None:
            errs.append(f"module {mid} {mo}: panel width not found ({sp})")
            continue
        x, y = m["pos"]
        if y != int(y) or x != int(x) or x < 0 or y < 0:
            errs.append(f"module {mid} {mo}: bad position {m['pos']}")
        boxes.append((mid, mo, x, y, x + hp))
    for i, a in enumerate(boxes):
        for b in boxes[i + 1:]:
            if a[3] == b[3] and a[2] < b[4] and b[2] < a[4]:
                errs.append(f"overlap: {a[0]} {a[1]} [{a[2]},{a[4]}) and {b[0]} {b[1]} [{b[2]},{b[4]}) in row {a[3]}")
    seen_in, seen_id = set(), set()
    for c in d.get("cables", []):
        if c.get("id") in seen_id:
            errs.append(f"duplicate cable id {c.get('id')}")
        seen_id.add(c.get("id"))
        for side, mk, pk, idx in (("output", "outputModuleId", "outputId", 2), ("input", "inputModuleId", "inputId", 1)):
            m = mods.get(c.get(mk))
            if m is None:
                errs.append(f"cable {c.get('id')}: {side} module {c.get(mk)} does not exist")
                continue
            cnt = COUNTS.get((m["plugin"], m["model"]))
            if cnt and not 0 <= c[pk] < cnt[idx]:
                errs.append(f"cable {c.get('id')}: {side} port {c[pk]} out of range on {m['model']} (0..{cnt[idx]-1})")
        key = (c.get("inputModuleId"), c.get("inputId"))
        if key in seen_in:
            errs.append(f"cable {c.get('id')}: input {key} already has a cable")
        seen_in.add(key)
        if not re.fullmatch(r"#[0-9a-f]{6}", c.get("color", "")):
            errs.append(f"cable {c.get('id')}: bad colour {c.get('color')}")
    notes = [m for m in d.get("modules", []) if m.get("model") == "Notes"]
    if len(notes) != 1:
        warns.append(f"{len(notes)} Notes modules")
    n = len(d.get("modules", []))
    if n > 15:
        warns.append(f"{n} modules (more than 15)")
    displays = [m for m in d.get("modules", []) if m.get("model") == "Display"]
    if len(displays) != 1:
        warns.append(f"{len(displays)} Display modules")
    info = f"{n} modules, {len(d.get('cables', []))} cables"
    return errs, warns, info

def main():
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, "patches", "[1-9]*.vcv")))
    models = load_models()
    bad = 0
    for f in files:
        r = check(f, models)
        errs, warns = r[0], r[1]
        info = r[2] if len(r) > 2 else ""
        status = "FAIL" if errs else "ok"
        print(f"{status:4} {os.path.basename(f)}  {info}")
        for e in errs:
            print("     ERROR", e)
        for w in warns:
            print("     warn ", w)
        bad += bool(errs)
    sys.exit(1 if bad else 0)

main()
