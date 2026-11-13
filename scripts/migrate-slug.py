#!/usr/bin/env python3
"""Migrate VCV Rack 2 patches saved with Video Research Unit 2.0.0 to the 2.1.0 module slugs.

Version 2.1.0 renamed two module slugs: Wobbulator is now DeflectionYoke and RuttEtra is now
ScanProcessor (see MODELS below). Patches, presets, selections and templates saved with 2.0.0 that
use those modules need their "model" fields rewritten before Rack can find them. The same script
also rewrites the older plugin slug "rasterlab" to "VideoResearchUnit" in files that still carry it.

What it does
  * Finds candidate files (see --help), parses the ones that name the old slug, and
    rewrites ONLY the "plugin" and "model" fields of module and preset records. It parses the JSON;
    it never does a text replace.
  * Handles plain JSON files (.vcvm presets, Rack's autosave patch.json) and Rack's
    zstd-compressed tar archives (.vrack/.vcv/.vcvs, and any other zstd tar). The
    archive is written back in the same format. Only JSON members are touched.
  * Backs every file up first, mirroring its original path, into
    ~/VRU-migration-backups/slug-migration-<timestamp>/ and writes restore.sh there.
  * Verifies each rewritten file by reading it back: no old slug left, and the
    document is identical to the original apart from the plugin fields.
  * Moves presets/rasterlab to presets/VideoResearchUnit, renames the module folders
    inside it that follow MODELS, renames Rack's
    pluginSettings key in settings.json, and renames the rasterlab-video-cache folder.
  * Is idempotent: a second run finds nothing and changes nothing.

Standard library only. Needs the `zstd` command for archives (brew install zstd).
Quit Rack before a real run, or Rack will overwrite settings.json and the autosave.
"""

import argparse
import copy
import datetime
import io
import json
import os
import shlex
import shutil
import subprocess
import sys
import tarfile

OLD = "rasterlab"
NEW = "VideoResearchUnit"
MODELS = {"Wobbulator": "DeflectionYoke", "RuttEtra": "ScanProcessor"}
TOKENS = [t.encode() for t in [OLD] + list(MODELS)]
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
EXTENSIONS = (".vrack", ".vcv", ".vcvs", ".vcvm")
HOME = os.path.expanduser("~")
DEFAULT_RACK_DIR = os.path.join(HOME, "Library", "Application Support", "Rack2")
DEFAULT_SCAN_ROOTS = [os.path.join(HOME, d) for d in ("Documents", "Music", "Desktop")]
SKIP_DIR_NAMES = {".git", "node_modules", ".Trash", "VRU-migration-backups", "Library"}
UNDO_ROOT = os.path.join(HOME, "VRU-migration-backups")


# --------------------------------------------------------------------------- JSON

def rewrite_plugin_fields(node):
    """Rewrite plugin and model slugs in module/preset records, in place. Returns the count of records changed."""
    count = 0
    if isinstance(node, dict):
        if node.get("plugin") in (OLD, NEW) and "model" in node:
            changed = False
            if node["plugin"] == OLD:
                node["plugin"] = NEW
                changed = True
            if node["model"] in MODELS:
                node["model"] = MODELS[node["model"]]
                changed = True
            if changed:
                count += 1
        for v in node.values():
            count += rewrite_plugin_fields(v)
    elif isinstance(node, list):
        for v in node:
            count += rewrite_plugin_fields(v)
    return count


def count_plugin_fields(node, slug=None):
    """Count records that still carry an old plugin slug or an old model slug."""
    n = 0
    if isinstance(node, dict):
        if node.get("plugin") in (OLD, NEW) and "model" in node:
            if node["plugin"] == OLD or node["model"] in MODELS:
                n += 1
        for v in node.values():
            n += count_plugin_fields(v)
    elif isinstance(node, list):
        for v in node:
            n += count_plugin_fields(v)
    return n


def dump_json(doc, like_text):
    """Serialise in the same layout as the original: indented if it had newlines, else compact."""
    if "\n" in like_text.strip():
        out = json.dumps(doc, indent=2, ensure_ascii=False)
    else:
        out = json.dumps(doc, separators=(",", ":"), ensure_ascii=False)
    if like_text.endswith("\n"):
        out += "\n"
    return out


def migrate_json_bytes(raw):
    """Return (new_bytes, modules_rewritten) or (None, 0) if there is nothing to do or it is not JSON."""
    if not any(t in raw for t in TOKENS):
        return None, 0
    try:
        text = raw.decode("utf-8")
        doc = json.loads(text)
    except (UnicodeDecodeError, ValueError):
        return None, 0
    original = copy.deepcopy(doc)
    n = rewrite_plugin_fields(doc)
    if n == 0:
        return None, 0
    new_text = dump_json(doc, text)
    # Verify in memory: parses, old slug gone from plugin fields, nothing else changed.
    check = json.loads(new_text)
    if count_plugin_fields(check, OLD) != 0:
        raise RuntimeError("old slug still present after rewrite")
    expect = copy.deepcopy(original)
    rewrite_plugin_fields(expect)
    if check != expect:
        raise RuntimeError("rewritten JSON differs from the original beyond the plugin fields")
    return new_text.encode("utf-8"), n


# --------------------------------------------------------------------------- archives

def zstd_run(args, data):
    p = subprocess.run(["zstd"] + args, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        raise RuntimeError("zstd failed: " + p.stderr.decode("utf-8", "replace").strip())
    return p.stdout


def read_archive(raw):
    """Return (tar bytes) for a zstd-compressed buffer."""
    return zstd_run(["-d", "-c", "-q"], raw)


def migrate_archive_bytes(raw):
    """Rewrite plugin fields inside a zstd tar. Returns (new_bytes, {member: count}) or (None, {})."""
    tar_bytes = read_archive(raw)
    if not any(t in tar_bytes for t in TOKENS):
        return None, {}
    src = tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:")
    members = src.getmembers()
    out_buf = io.BytesIO()
    changed = {}
    contents = {}
    for m in members:
        data = src.extractfile(m).read() if m.isfile() else None
        if data is not None and m.name.endswith(".json"):
            new, n = migrate_json_bytes(data)
            if new is not None:
                data = new
                changed[m.name] = n
        contents[m.name] = data
    if not changed:
        return None, {}
    with tarfile.open(fileobj=out_buf, mode="w:", format=src.format) as dst:
        for m in members:
            data = contents[m.name]
            if data is not None:
                m = copy.copy(m)
                m.size = len(data)
                dst.addfile(m, io.BytesIO(data))
            else:
                dst.addfile(m)
    new_raw = zstd_run(["-c", "-q", "-3"], out_buf.getvalue())
    # Verify by re-reading the compressed result.
    back = tarfile.open(fileobj=io.BytesIO(read_archive(new_raw)), mode="r:")
    back_names = [m.name for m in back.getmembers()]
    if back_names != [m.name for m in members]:
        raise RuntimeError("archive members differ after rewrite")
    for m in back.getmembers():
        if not m.isfile():
            continue
        data = back.extractfile(m).read()
        if m.name in changed:
            if count_plugin_fields(json.loads(data.decode("utf-8")), OLD) != 0:
                raise RuntimeError("old slug still present in " + m.name)
        elif data != contents[m.name]:
            raise RuntimeError("member changed unexpectedly: " + m.name)
    return new_raw, changed


# --------------------------------------------------------------------------- candidates

def is_zstd(path):
    with open(path, "rb") as f:
        return f.read(4) == ZSTD_MAGIC


def candidate_files(paths, searched):
    """Yield files to examine under the given files/folders."""
    for p in paths:
        p = os.path.abspath(os.path.expanduser(p))
        if os.path.isfile(p):
            searched.append(p)
            yield p
        elif os.path.isdir(p):
            searched.append(p + "/ (recursive)")
            for root, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs if d not in SKIP_DIR_NAMES and not d.startswith(".")]
                for name in files:
                    if name.lower().endswith(EXTENSIONS) or name.endswith(".json"):
                        yield os.path.join(root, name)
        else:
            searched.append(p + " (not found)")


def rack_candidates(rack_dir, searched):
    """Rack's own folders, with the file kinds each can hold."""
    out = []
    auto = os.path.join(rack_dir, "autosave")
    if os.path.isdir(auto):
        searched.append(auto + "/ (patch*.json, modules/*.json)")
        for root, dirs, files in os.walk(auto):
            for name in files:
                if name.endswith(".json"):
                    out.append(os.path.join(root, name))
    else:
        searched.append(auto + " (not found)")
    for sub in ("patches", "selections", os.path.join("presets", OLD), os.path.join("presets", NEW)):
        d = os.path.join(rack_dir, sub)
        if os.path.isdir(d):
            searched.append(d + "/ (recursive)")
            for root, dirs, files in os.walk(d):
                for name in files:
                    if name.lower().endswith(EXTENSIONS) or name.endswith(".json"):
                        out.append(os.path.join(root, name))
        else:
            searched.append(d + " (not found)")
    tpl = os.path.join(rack_dir, "templates", "template.vcv")
    if os.path.isfile(tpl):
        searched.append(tpl)
        out.append(tpl)
    else:
        searched.append(tpl + " (not found)")
    return out


def examine(path):
    """Return a plan dict for one file, or None if it needs no change."""
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        print("  cannot read %s: %s" % (path, e), file=sys.stderr)
        return None
    if raw[:4] == ZSTD_MAGIC:
        try:
            new, changed = migrate_archive_bytes(raw)
        except Exception as e:  # noqa: BLE001 - report and skip, never half-write
            print("  skipped (not a readable archive: %s): %s" % (e, path), file=sys.stderr)
            return None
        if new is None:
            return None
        return {"path": path, "kind": "archive", "new": new, "modules": sum(changed.values()), "detail": changed}
    new, n = migrate_json_bytes(raw)
    if new is None:
        return None
    return {"path": path, "kind": "json", "new": new, "modules": n, "detail": {"": n}}


# --------------------------------------------------------------------------- backup / apply

def mirror(backup_dir, path):
    return os.path.join(backup_dir, "files", path.lstrip("/"))


def backup_path(backup_dir, path):
    dst = mirror(backup_dir, path)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.isdir(path):
        shutil.copytree(path, dst, symlinks=True)
    else:
        shutil.copy2(path, dst)
    return dst


def atomic_write(path, data):
    tmp = path + ".slugtmp"
    st = os.stat(path)
    with open(tmp, "wb") as f:
        f.write(data)
    os.chmod(tmp, st.st_mode & 0o7777)
    os.replace(tmp, path)


def verify_written(plan):
    with open(plan["path"], "rb") as f:
        raw = f.read()
    if plan["kind"] == "archive":
        new, changed = migrate_archive_bytes(raw)
        if new is not None:
            raise RuntimeError("old slug still present in " + plan["path"])
    else:
        if any(t in raw for t in TOKENS):
            doc = json.loads(raw.decode("utf-8"))
            if count_plugin_fields(doc, OLD):
                raise RuntimeError("old slug still present in " + plan["path"])


# --------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(
        description='Rewrite old plugin and module slugs in Rack patches, presets, '
                    'selections and templates, with backups and a restore script.')
    ap.add_argument("paths", nargs="*", help="files or folders to migrate. Default: Rack's own folders "
                    "(autosave, patches, presets/rasterlab, selections, templates/template.vcv, settings.json) "
                    "and .vrack/.vcv/.vcvs/.vcvm files under ~/Documents, ~/Music and ~/Desktop.")
    ap.add_argument("--dry-run", action="store_true", help="report what would change; write nothing")
    ap.add_argument("--rack-dir", default=DEFAULT_RACK_DIR, help="Rack user folder (default: %(default)s)")
    ap.add_argument("--undo-root", default=UNDO_ROOT, help="where backups go (default: %(default)s)")
    ap.add_argument("--no-home-scan", action="store_true",
                    help="default mode, but skip ~/Documents, ~/Music and ~/Desktop (used by the tests)")
    ap.add_argument("--ignore-running", action="store_true", help="do not refuse when Rack is running (used by the tests)")
    ap.add_argument("--timestamp", help="timestamp for the backup folder name (default: now)")
    args = ap.parse_args()

    ts = args.timestamp or datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup_dir = os.path.join(args.undo_root, "slug-migration-" + ts)
    rack_dir = os.path.abspath(args.rack_dir)
    searched = []
    use_defaults = not args.paths

    if use_defaults:
        cands = rack_candidates(rack_dir, searched)
        if not args.no_home_scan:
            cands += list(candidate_files(DEFAULT_SCAN_ROOTS, searched))
    else:
        cands = list(candidate_files(args.paths, searched))
    seen = set()
    cands = [c for c in cands if not (c in seen or seen.add(c))]

    print("Searched:")
    for s in searched:
        print("  " + s)
    print("Examined %d candidate file(s)." % len(cands))

    plans = []
    for c in cands:
        # settings.json is handled separately below.
        plan = examine(c)
        if plan:
            plans.append(plan)

    # Rack extras: presets folder, settings.json key, video cache.
    presets_old = os.path.join(rack_dir, "presets", OLD)
    presets_new = os.path.join(rack_dir, "presets", NEW)
    do_presets = use_defaults and os.path.isdir(presets_old)
    settings_path = os.path.join(rack_dir, "settings.json")
    settings_plan = None
    if use_defaults and os.path.isfile(settings_path):
        try:
            with open(settings_path, "rb") as f:
                sraw = f.read()
            sdoc = json.loads(sraw.decode("utf-8"))
            ps = sdoc.get("pluginSettings", {})
            if isinstance(ps, dict) and OLD in ps:
                if NEW in ps:
                    print("settings.json: pluginSettings has both %s and %s; leaving it alone." % (OLD, NEW))
                else:
                    ps[NEW] = ps.pop(OLD)
                    settings_plan = {"path": settings_path, "new": dump_json(sdoc, sraw.decode("utf-8")).encode()}
        except (OSError, ValueError) as e:
            print("settings.json unreadable: %s" % e, file=sys.stderr)
    models_dir = presets_new if os.path.isdir(presets_new) else presets_old
    old_model_dirs = [m for m in MODELS if use_defaults and os.path.isdir(os.path.join(models_dir, m))]
    do_model_dirs = bool(old_model_dirs)
    cache_old = os.path.join(rack_dir, OLD + "-video-cache")
    cache_new = os.path.join(rack_dir, NEW + "-video-cache")
    do_cache = use_defaults and os.path.isdir(cache_old) and not os.path.exists(cache_new)

    total_modules = sum(p["modules"] for p in plans)
    print()
    if not plans and not do_presets and not settings_plan and not do_cache and not do_model_dirs:
        print("Nothing to migrate: no file names the old plugin or module slugs.")
        return 0

    print(("DRY RUN. Would change" if args.dry_run else "Changing") + " %d file(s), %d module/preset record(s):"
          % (len(plans), total_modules))
    for p in plans:
        print("  %-8s %3d record(s)  %s" % (p["kind"], p["modules"], p["path"]))
        if p["kind"] == "archive":
            for member, n in p["detail"].items():
                print("             %3d in %s" % (n, member))
    if settings_plan:
        print("  settings  pluginSettings.%s -> pluginSettings.%s  %s" % (OLD, NEW, settings_path))
    if do_presets:
        print("  folder   %s -> %s" % (presets_old, presets_new))
    if do_cache:
        print("  folder   %s -> %s" % (cache_old, cache_new))
    for m in old_model_dirs:
        print("  folder   %s -> %s" % (m, MODELS[m]))

    if args.dry_run:
        print("\nDry run only. Nothing was written. Backup folder would be:\n  " + backup_dir)
        return 0

    # Refuse to touch Rack's files while Rack runs.
    if use_defaults and not args.ignore_running:
        r = subprocess.run(["pgrep", "-f", "VCV Rack|/Rack$|Rack2 Pro|Rack Free|Rack Pro"], stdout=subprocess.PIPE)
        mine = {str(os.getpid()), str(os.getppid())}
        running = [x for x in r.stdout.decode().split() if x not in mine]
        if running:
            print("\nRack appears to be running (pid %s). Quit Rack first so it does not overwrite the "
                  "migrated files. Aborting; nothing was written." % ",".join(running), file=sys.stderr)
            return 2

    os.makedirs(backup_dir, exist_ok=True)
    restore = ["#!/bin/sh", "# Undo slug migration " + ts + ". Run: sh " + shlex.quote(os.path.join(backup_dir, "restore.sh")),
               "set -e"]
    manifest = []

    for p in plans:
        if (do_presets and p["path"].startswith(presets_old + os.sep)) or \
                (do_model_dirs and p["path"].startswith(presets_new + os.sep)):
            manifest.append({"file": p["path"], "backup": "covered by the presets folder backup", "records": p["modules"]})
            continue  # the whole folder is backed up and restored below
        b = backup_path(backup_dir, p["path"])
        restore.append("cp -p %s %s" % (shlex.quote(b), shlex.quote(p["path"])))
        manifest.append({"file": p["path"], "backup": b, "records": p["modules"]})
    if settings_plan:
        b = backup_path(backup_dir, settings_path)
        restore.append("cp -p %s %s" % (shlex.quote(b), shlex.quote(settings_path)))
        manifest.append({"file": settings_path, "backup": b, "records": 0})
    if do_presets:
        b = backup_path(backup_dir, presets_old)
        manifest.append({"file": presets_old, "backup": b, "records": 0})
    if do_model_dirs and not do_presets:
        b = backup_path(backup_dir, presets_new)
        restore.append("rm -rf %s" % shlex.quote(presets_new))
        restore.append("cp -a %s %s" % (shlex.quote(b), shlex.quote(presets_new)))
        manifest.append({"file": presets_new, "backup": b, "records": 0})
    with open(os.path.join(backup_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    # Write restore.sh early (folder moves appended below) so a crash still leaves a way back.
    def flush_restore():
        with open(os.path.join(backup_dir, "restore.sh"), "w") as f:
            f.write("\n".join(restore) + "\n")
        os.chmod(os.path.join(backup_dir, "restore.sh"), 0o755)
    flush_restore()

    try:
        for p in plans:
            atomic_write(p["path"], p["new"])
            verify_written(p)
        if settings_plan:
            atomic_write(settings_path, settings_plan["new"])
        if do_presets:
            if os.path.exists(presets_new):
                for name in os.listdir(presets_old):
                    dst = os.path.join(presets_new, name)
                    if os.path.exists(dst):
                        print("  presets: %s exists in the new folder; left in the old folder" % name)
                    else:
                        shutil.move(os.path.join(presets_old, name), dst)
                try:
                    os.rmdir(presets_old)
                except OSError:
                    pass
            else:
                os.rename(presets_old, presets_new)
            restore.append("rm -rf %s" % shlex.quote(presets_new))
            restore.append("rm -rf %s" % shlex.quote(presets_old))
            restore.append("cp -a %s %s" % (shlex.quote(mirror(backup_dir, presets_old)), shlex.quote(presets_old)))
        if do_model_dirs:
            base = presets_new if os.path.isdir(presets_new) else presets_old
            for m in old_model_dirs:
                src = os.path.join(base, m)
                dst = os.path.join(base, MODELS[m])
                if os.path.isdir(src):
                    if os.path.exists(dst):
                        for name in os.listdir(src):
                            if not os.path.exists(os.path.join(dst, name)):
                                shutil.move(os.path.join(src, name), os.path.join(dst, name))
                        shutil.rmtree(src, ignore_errors=True)
                    else:
                        os.rename(src, dst)
        if do_cache:
            os.rename(cache_old, cache_new)
            restore.append("mv %s %s" % (shlex.quote(cache_new), shlex.quote(cache_old)))
        flush_restore()
    except Exception as e:  # noqa: BLE001
        print("\nERROR: %s\nRestoring everything from the backup." % e, file=sys.stderr)
        flush_restore()
        subprocess.run(["sh", os.path.join(backup_dir, "restore.sh")])
        return 1

    print("\nSummary")
    print("  files changed:    %d" % (len(plans) + (1 if settings_plan else 0)))
    print("  records rewritten: %d" % total_modules)
    for p in plans:
        print("    %3d  %s" % (p["modules"], p["path"]))
    print("  backup folder:    " + backup_dir)
    print("  restore with:     sh " + shlex.quote(os.path.join(backup_dir, "restore.sh")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
