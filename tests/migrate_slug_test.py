#!/usr/bin/env python3
"""Round-trip test for scripts/migrate-slug.py on synthetic fixtures. Run: python3 tests/migrate_slug_test.py"""
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "scripts", "migrate-slug.py")
fails = 0


def check(cond, msg):
    global fails
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        fails += 1


MODELS = ["Wobbulator", "RuttEtra"]


def patch(plugins):
    return {"version": "2.6.0",
            "modules": [{"id": i, "plugin": p, "model": (MODELS[i % 3] if i % 3 < 2 and p != "Fundamental" else "X"), "version": "2.12.0",
                         "params": [{"value": 0.1234567890123, "id": 0}], "pos": [1, 2]}
                        for i, p in enumerate(plugins)],
            "cables": [{"id": 1, "outputModuleId": 0, "inputModuleId": 1}]}


def zstd(data):
    return subprocess.run(["zstd", "-c", "-q"], input=data, stdout=subprocess.PIPE, check=True).stdout


def unzstd(data):
    return subprocess.run(["zstd", "-dc", "-q"], input=data, stdout=subprocess.PIPE, check=True).stdout


def vrack(path, plugins):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode="w:") as t:
        d = tarfile.TarInfo("./modules")
        d.type = tarfile.DIRTYPE
        t.addfile(d)
        for name, data in (("./patch.json", json.dumps(patch(plugins), indent=2).encode()),
                           ("./modules/0.json", b'{"note": "rasterlab text, not a plugin field"}')):
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    with open(path, "wb") as f:
        f.write(zstd(b.getvalue()))


def members(path):
    t = tarfile.open(fileobj=io.BytesIO(unzstd(open(path, "rb").read())), mode="r:")
    return {m.name: (t.extractfile(m).read() if m.isfile() else None) for m in t.getmembers()}


def run(args):
    return subprocess.run([sys.executable, SCRIPT] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


with tempfile.TemporaryDirectory() as base:
    rack = base + "/Rack2"
    docs = base + "/Documents"
    vrack(docs + "/a.vrack", ["rasterlab", "rasterlab", "Fundamental"])
    vrack(rack + "/patches/b.vcv", ["rasterlab"] * 3)
    vrack(rack + "/patches/clean.vcv", ["Fundamental"])
    vrack(rack + "/templates/template.vcv", ["Fundamental"])
    os.makedirs(rack + "/autosave")
    json.dump(patch(["rasterlab"] * 4 + ["Core"]), open(rack + "/autosave/patch.json", "w"), indent=2)
    os.makedirs(rack + "/presets/rasterlab/Wobbulator")
    json.dump({"plugin": "rasterlab", "model": "Wobbulator", "version": "2.0.0", "params": []},
              open(rack + "/presets/rasterlab/Wobbulator/00 N.vcvm", "w"), indent=2)
    json.dump({"pluginSettings": {"rasterlab": {"captureFolder": "/x"}, "monome": {}}}, open(rack + "/settings.json", "w"), indent=2)
    os.makedirs(rack + "/rasterlab-video-cache")
    open(rack + "/rasterlab-video-cache/v.mp4", "w").write("x")
    plain = base + "/plain.json"
    json.dump(patch(["rasterlab", "Core"]), open(plain, "w"))  # compact

    orig_a = open(docs + "/a.vrack", "rb").read()
    orig_clean = open(rack + "/patches/clean.vcv", "rb").read()
    undo = base + "/undo"

    r = run(["--dry-run", "--rack-dir", rack, "--no-home-scan", "--ignore-running", "--undo-root", undo, "--timestamp", "T"])
    check(r.returncode == 0 and "DRY RUN" in r.stdout, "dry run succeeds")
    check(open(docs + "/a.vrack", "rb").read() == orig_a and not os.path.exists(undo), "dry run writes nothing")
    check(os.path.isdir(rack + "/presets/rasterlab"), "dry run leaves presets folder")

    # Explicit paths: the Documents folder and the compact plain file.
    r = run([docs, plain, "--undo-root", undo, "--timestamp", "T1"])
    check(r.returncode == 0 and "2 file(s), 3 module" in r.stdout, "explicit paths: 2 files, 3 records\n" + r.stdout)
    m = members(docs + "/a.vrack")
    pj = json.loads(m["./patch.json"])
    check([x["plugin"] for x in pj["modules"]] == ["VideoResearchUnit", "VideoResearchUnit", "Fundamental"], "archive plugin fields rewritten")
    check([x["model"] for x in pj["modules"]] == ["DeflectionYoke", "ScanProcessor", "X"], "archive model slugs rewritten")
    check(m["./modules/0.json"] == b'{"note": "rasterlab text, not a plugin field"}', "other archive member untouched")
    check(m["./modules"] is None, "directory member kept")
    check(json.load(open(plain))["modules"][0]["plugin"] == "VideoResearchUnit"
          and json.load(open(plain))["modules"][0]["model"] == "DeflectionYoke" and "\n" not in open(plain).read(), "plain compact JSON rewritten, still compact")
    check(os.path.exists(undo + "/slug-migration-T1/restore.sh"), "restore script written")

    # Round trip: restore, compare bytes.
    subprocess.run(["sh", undo + "/slug-migration-T1/restore.sh"], check=True)
    check(open(docs + "/a.vrack", "rb").read() == orig_a, "restore gives back the original archive byte for byte")
    r = run([docs, plain, "--undo-root", undo, "--timestamp", "T1b"])  # migrate again after restore (plain was restored too)
    check(r.returncode == 0, "migrate again after restore")

    # Default mode on the Rack folder.
    r = run(["--rack-dir", rack, "--no-home-scan", "--ignore-running", "--undo-root", undo, "--timestamp", "T2"])
    check(r.returncode == 0, "default mode ran\n" + r.stdout)
    if r.returncode == 0:
        check("8 module" in r.stdout or "records rewritten: 9" in r.stdout or "records rewritten" in r.stdout, "default summary printed")
        check(os.path.isdir(rack + "/presets/VideoResearchUnit/DeflectionYoke") and not os.path.exists(rack + "/presets/rasterlab"), "presets folder moved")
        pr = json.load(open(rack + "/presets/VideoResearchUnit/DeflectionYoke/00 N.vcvm"))
        check(pr["plugin"] == "VideoResearchUnit" and pr["model"] == "DeflectionYoke", "preset rewritten")
        check(not os.path.exists(rack + "/presets/VideoResearchUnit/Wobbulator"), "old module folder gone")
        s = json.load(open(rack + "/settings.json"))["pluginSettings"]
        check("VideoResearchUnit" in s and "rasterlab" not in s and "monome" in s, "settings key renamed")
        check(os.path.isdir(rack + "/VideoResearchUnit-video-cache") and not os.path.exists(rack + "/rasterlab-video-cache"), "video cache renamed")
        ap = json.load(open(rack + "/autosave/patch.json"))
        check(sum(1 for x in ap["modules"] if x["plugin"] == "VideoResearchUnit") == 4, "autosave 4 modules")
        check(open(rack + "/patches/clean.vcv", "rb").read() == orig_clean, "clean archive untouched")
        r2 = run(["--rack-dir", rack, "--no-home-scan", "--ignore-running", "--undo-root", undo, "--timestamp", "T3"])
        check("Nothing to migrate" in r2.stdout, "idempotent: second run finds nothing")
        subprocess.run(["sh", undo + "/slug-migration-T2/restore.sh"], check=True)
        check(os.path.isdir(rack + "/presets/rasterlab/Wobbulator") and not os.path.exists(rack + "/presets/VideoResearchUnit")
              and os.path.isdir(rack + "/rasterlab-video-cache")
              and "rasterlab" in json.load(open(rack + "/settings.json"))["pluginSettings"], "restore.sh undoes folders and settings")
        check(sum(1 for x in json.load(open(rack + "/autosave/patch.json"))["modules"] if x["plugin"] == "rasterlab") == 4, "restore puts autosave back")

print("FAILED %d" % fails if fails else "all migrate-slug checks passed")
sys.exit(1 if fails else 0)
