#!/bin/sh
# Rebuilds Frameworks/RLSyphon.framework from the pinned Syphon-Framework source.
#
# Steps:
#   1. clone https://github.com/Syphon/Syphon-Framework at a pinned commit into build/syphon/src
#   2. rename every Syphon identifier to RLSyphon (scripts/syphon_rename.py), so the
#      vendored copy cannot collide with another Syphon.framework in the same process
#   3. drop the Metal shader file from the target (it needs the Metal Toolchain
#      component, and the plugin only uses Syphon's OpenGL server)
#   4. build arm64 Release with xcodebuild into build/syphon/out
#   5. copy the bundle into Frameworks/ and ad-hoc sign the binary
#
# Requires Xcode command line tools and python3. Run from anywhere.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SYPHON_REPO=https://github.com/Syphon/Syphon-Framework.git
SYPHON_COMMIT=f4761677a45b8034a3c2069ec0f3d2553da81fba
WORK="$ROOT/build/syphon"
SRC="$WORK/src"
OUT="$WORK/out"
DEST="$ROOT/Frameworks/RLSyphon.framework"

rm -rf "$SRC" "$OUT"
mkdir -p "$WORK"
git clone --quiet "$SYPHON_REPO" "$SRC"
git -C "$SRC" checkout --quiet "$SYPHON_COMMIT"
rm -rf "$SRC/.git"

python3 "$ROOT/scripts/syphon_rename.py" "$SRC"

sed -i '' '/RLSyphonMetalShaders.metal in Sources \*\//d' "$SRC/Syphon.xcodeproj/project.pbxproj"

xcodebuild -project "$SRC/Syphon.xcodeproj" -target Syphon -configuration Release \
	ARCHS=arm64 ONLY_ACTIVE_ARCH=YES CODE_SIGNING_ALLOWED=NO \
	MACOSX_DEPLOYMENT_TARGET=12.0 SYMROOT="$OUT" build

BUILT="$OUT/Release/RLSyphon.framework"
rm -rf "$DEST"
mkdir -p "$DEST/Versions/A"
cp -R "$BUILT/Versions/A/" "$DEST/Versions/A/"
# Standard framework symlinks. The linker and the headers use the top-level names.
ln -s A "$DEST/Versions/Current"
ln -s Versions/Current/RLSyphon "$DEST/RLSyphon"
ln -s Versions/Current/Headers "$DEST/Headers"
ln -s Versions/Current/Modules "$DEST/Modules"
ln -s Versions/Current/Resources "$DEST/Resources"

cp "$SRC/License.txt" "$ROOT/Frameworks/RLSyphon-LICENSE.txt"
# Seal the whole bundle so the resource seal matches the renamed binary and Info.plist.
codesign --force --sign - "$DEST"

echo "Built $DEST from Syphon-Framework $SYPHON_COMMIT"
