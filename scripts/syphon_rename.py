#!/usr/bin/env python3
"""Rename the Syphon.framework public and private symbols to an RLSyphon prefix.

This plugin bundles its own copy of Syphon.framework inside the plugin folder.
Another Rack plugin may bundle a different copy, and two copies loaded into one
Rack process would register duplicate Objective-C classes and C symbols.  This
script gives every Syphon identifier the prefix "RLSyphon" so the two copies
cannot collide.

What is renamed:
  * identifiers that start with "Syphon" followed by an uppercase letter
    (classes, C functions, constants, macros, include guards), in code;
  * "Syphon" as the framework, module and umbrella name in build and module files;
  * source and header file names that start with "Syphon", plus Syphon.h and
    Syphon.modulemap.

What is NOT renamed:
  * the contents of string literals.  Syphon's wire protocol uses literal
    strings such as "info.v002.Syphon.ServerAnnounce" and
    "SyphonServerDescriptionNameKey" in distributed notifications and server
    description dictionaries.  Changing them would hide this plugin's servers from
    OBS, Resolume and other Syphon clients.  Those strings stay byte-for-byte.
  * the licence and README text.

Usage: syphon_rename.py <syphon-source-root>
"""
import os
import re
import sys

IDENT = re.compile(r"Syphon(?=[A-Z])")
PREFIX = "RLSyphon"
CODE_EXTS = {".h", ".m", ".mm", ".c", ".metal", ".pch"}
SKIP_DIRS = {".git", "Syphon.docc", "en.lproj"}
KEEP_FILES = {"LICENSE", "License.txt", "README.md"}


def rename_include(line):
    """Rename a #import/#include line, where the file names are the identifiers."""
    line = line.replace("<Syphon/", "<RLSyphon/")
    line = line.replace('"Syphon.h"', '"RLSyphon.h"')
    return IDENT.sub(PREFIX, line)


def skip_quoted(line, i, quote):
    """Return the index just past the literal that starts at line[i]."""
    j = i + 1
    while j < len(line) and line[j] != quote:
        j += 2 if line[j] == "\\" else 1
    return j + 1


def rename_code_line(line):
    """Rename identifiers on one line of C or ObjC, skipping string and char literals."""
    stripped = line.lstrip()
    if stripped.startswith("#import") or stripped.startswith("#include"):
        return rename_include(line)
    out = []
    i = 0
    n = len(line)
    while i < n:
        ch = line[i]
        if ch in "\"'":
            end = skip_quoted(line, i, ch)
            out.append(line[i:end])
            i = end
            continue
        if line.startswith("//", i):
            out.append(IDENT.sub(PREFIX, line[i:]))
            break
        j = i
        while j < n and line[j] not in "\"'" and not line.startswith("//", j):
            j += 1
        out.append(IDENT.sub(PREFIX, line[i:j]))
        i = j
    return "".join(out)


def rename_code(text):
    return "\n".join(rename_code_line(line) for line in text.split("\n"))


def rename_build_file(text):
    """Rename framework, module and umbrella names in build and module files."""
    text = re.sub(r"\bmodule Syphon \{", "module RLSyphon {", text)
    text = re.sub(r'umbrella header "Syphon\.h"', 'umbrella header "RLSyphon.h"', text)
    text = re.sub(r"\bSyphon\.(framework|h|modulemap)\b", r"RLSyphon.\1", text)
    text = re.sub(r"\bSyphon_Prefix\.pch\b", "RLSyphon_Prefix.pch", text)
    text = re.sub(r"(PRODUCT_NAME = )Syphon;", r"\1RLSyphon;", text)
    return IDENT.sub(PREFIX, text)


def rewrite(path, transform):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    new = transform(text)
    if new != text:
        with open(path, "w", encoding="utf-8") as f:
            f.write(new)


def main():
    root = os.path.abspath(sys.argv[1])
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if name in KEEP_FILES:
                continue
            path = os.path.join(dirpath, name)
            ext = os.path.splitext(name)[1]
            if name == "project.pbxproj" or name == "Syphon.modulemap" or ext == ".exp":
                rewrite(path, rename_build_file)
            elif ext in CODE_EXTS:
                rewrite(path, rename_code)

    renamed = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            ext = os.path.splitext(name)[1]
            new_name = None
            if name in ("Syphon.h", "Syphon.modulemap"):
                new_name = "RL" + name
            elif name.startswith("Syphon") and ext in CODE_EXTS:
                new_name = "RL" + name
            if new_name:
                os.rename(os.path.join(dirpath, name), os.path.join(dirpath, new_name))
                renamed += 1
    print("renamed %d files" % renamed)


if __name__ == "__main__":
    main()
