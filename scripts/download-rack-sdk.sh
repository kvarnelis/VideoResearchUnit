#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-2.6.6}"
ARCH="${2:-$(uname -m)}"

case "$ARCH" in
  arm64)
    SDK_ARCH="arm64"
    ;;
  x86_64)
    SDK_ARCH="x64"
    ;;
  *)
    echo "Unsupported architecture: $ARCH" >&2
    exit 1
    ;;
esac

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ZIP_PATH="${TMPDIR:-/tmp}/Rack-SDK-${VERSION}-mac-${SDK_ARCH}.zip"
URL="https://vcvrack.com/downloads/Rack-SDK-${VERSION}-mac-${SDK_ARCH}.zip"

cd "$ROOT_DIR"

if [[ -d Rack-SDK ]]; then
  echo "Rack-SDK already exists at $ROOT_DIR/Rack-SDK"
  echo "Remove it first if you want to replace it."
  exit 0
fi

echo "Downloading $URL"
curl -L -o "$ZIP_PATH" "$URL"

echo "Unpacking $ZIP_PATH"
unzip -q "$ZIP_PATH"

echo "Ready. Build with:"
echo "  export RACK_DIR=\"$ROOT_DIR/Rack-SDK\""
echo "  make"
echo "  make dist"
