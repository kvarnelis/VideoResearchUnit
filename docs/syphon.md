# Syphon implementation notes

VRU has `Syphon In` (VRU–08) and `Syphon Out` (VRU–17) Rack modules and a bridge boundary in
`src/syphon_bridge.hpp`. On Apple silicon the bridge is linked by default and talks
to the vendored `Frameworks/RLSyphon.framework`. Other builds (`RASTERLAB_SYPHON=0`)
use null bridge objects that report `NO SYPHON`.

## Production choices

Use Syphon for Rack-to-visual-app interop:

- `SyphonInputBridge` enumerates available servers and copies the latest frame
  into an external-frame bus node.
- `SyphonOutputBridge` publishes an OpenGL texture uploaded from the resolved
  video bus.

For Rack, OpenGL is the lowest-friction first implementation because Rack's UI
already uses OpenGL/NanoVG. A later standalone Mac app should use Metal.

## Why no direct camera input first

Camera capture inside a Rack plugin is fragile on macOS because the permission
prompt and usage string belong to the host application bundle. Syphon input lets
OBS, VDMX, Black Syphon, or another capture app own camera permissions while
Rack receives frames as a texture stream.

## Current slice

1. `OpenGlVideoTexture` uploads CPU RGBA bus frames to `GL_TEXTURE_2D`.
2. `Syphon Out` renders the selected bus at 1024x768 (default) or 1440x1080, from
   its right-click menu, and publishes that texture. The server name is
   `VRU – <module id>`, or a `serverLabel` stored in the patch.
3. `Syphon In` has no automatic choice. Its right-click menu lists the running
   servers, plus `None`. The chosen name is stored in the patch as
   `selectedServerName`. Frames are copied to an external-frame store and emitted
   as a bus token.
4. Status screens show `NO SYPHON` when the bridge is not linked, `LINKED` when it
   is ready but idle, and the server name while publishing or receiving.

Frames are top-down RGBA, like the CPU renderer. The bridge publishes with
`flipped = true`. Syphon draws the frame upright in its surface, and `Syphon In`
reads it back into top-down rows.

## Vendored framework

`Frameworks/RLSyphon.framework` is built by `scripts/build-syphon-framework.sh`:

- Source: `https://github.com/Syphon/Syphon-Framework`, pinned to commit
  `f4761677a45b8034a3c2069ec0f3d2553da81fba` (BSD licence, copied to
  `Frameworks/RLSyphon-LICENSE.txt`).
- Prefix: `scripts/syphon_rename.py` renames every `Syphon<Uppercase>` identifier,
  the framework, module and umbrella names, and the Syphon source and header file
  names to `RLSyphon`. This keeps two copies of Syphon in one Rack process from
  colliding, both at class level and at C symbol level.
- Wire protocol unchanged: string literals are not renamed. The notification and
  server description strings (for example `info.v002.Syphon.ServerAnnounce` and
  `SyphonServerDescriptionNameKey`) must stay as they are, or OBS, Resolume and
  other Syphon clients cannot see VRU servers.
- Build: arm64 only, Release, `MACOSX_DEPLOYMENT_TARGET=12.0`. The Metal shader
  file is left out because it needs the Metal Toolchain component. VRU uses
  only Syphon's OpenGL server.
- Signing: ad-hoc (`codesign -s -`) for development and packaging. A release
  would need Developer ID signing and Apple notarization with your own
  Apple Developer account.

## Linking

- The plugin links `@rpath/RLSyphon.framework/Versions/A/RLSyphon`. Its rpath is
  `@loader_path/Frameworks`, and its install name is `@rpath/plugin.dylib`.
- `make dist` copies the versioned binary, its `Info.plist`, its seal, its headers
  and module map, and the licence into `dist/VideoResearchUnit/Frameworks/`. The framework's
  top-level symlinks are not shipped because plugin.mk's `rsync -r` does not copy
  symlinks. The loader path does not need them.
- Tests: `make test` builds `tests/syphon_roundtrip.mm`, which runs the real
  bridges in one process on an offscreen legacy GL context. It publishes a
  patterned frame and checks that the received bytes match exactly.

## Video input recommendation

Use Syphon input before direct camera input.

For the Rack plugin, a `Syphon In` module should be the first video-input
module. It can enumerate Syphon servers, receive the selected frame stream, and
publish that stream into the same video-bus registry used by `Video Oscillator`.

Direct camera capture can wait until either:

- a separate standalone Mac app exists, or
- there is a clear reason to handle camera permissions and capture devices from
  inside the Rack host.

This keeps Rack focused on CV patching and avoids making the plugin depend on
host-level macOS camera permission behavior.

## Native macOS bridge

`src/syphon_bridge_mac.mm` is compiled when `RASTERLAB_SYPHON=1`, which is the
default on Apple silicon. It imports `<RLSyphon/RLSyphon.h>` and links
Cocoa/OpenGL and the vendored framework. Input reads back frames with a framebuffer
and `glReadPixels`. `glGetTexImage` on an IOSurface-backed texture returned only
one row in testing.

## No audio

Syphon carries pictures only. It has no audio channel, and Syphon Out does not take the Display's L and R
audio inputs (those feed REC, see docs/architecture.md, "Audio for REC"). To hear or record Rack's sound
beside a Syphon picture, send Rack's audio out through your audio interface or a loopback device and
capture that in the receiving app.
