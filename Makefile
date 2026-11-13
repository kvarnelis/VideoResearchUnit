RACK_DIR ?= ./Rack-SDK

SLUG = VideoResearchUnit
VERSION = 2.1.0

FLAGS += -std=c++17
FLAGS += -Iinclude
SOURCES += $(wildcard src/*.cpp)

# Syphon is linked by default on Apple silicon Macs. Other hosts build without it.
# Set RASTERLAB_SYPHON=0 to build the null bridge.
ifeq ($(shell uname -sm),Darwin arm64)
RASTERLAB_SYPHON ?= 1
else
RASTERLAB_SYPHON ?= 0
endif

SYPHON_FRAMEWORK = Frameworks/RLSyphon.framework

ifeq ($(RASTERLAB_SYPHON),1)
FLAGS += -DRASTERLAB_WITH_SYPHON=1
FLAGS += -F$(CURDIR)/Frameworks
SOURCES += src/syphon_bridge_mac.mm
LDFLAGS += -mmacosx-version-min=12.0
LDFLAGS += -framework Cocoa -framework OpenGL
LDFLAGS += -F$(CURDIR)/Frameworks -framework RLSyphon
# plugin.dylib finds the framework next to itself, wherever the plugin folder is installed.
LDFLAGS += -Wl,-install_name,@rpath/plugin.dylib
LDFLAGS += -Wl,-rpath,@loader_path/Frameworks
# plugin.mk's dist step copies with rsync -r, which drops framework symlinks, so ship the
# versioned binary and its Info.plist. The loader path above resolves to exactly these files.
DISTRIBUTABLES += $(SYPHON_FRAMEWORK)/Versions/A/RLSyphon
DISTRIBUTABLES += $(SYPHON_FRAMEWORK)/Versions/A/Resources/Info.plist
DISTRIBUTABLES += $(SYPHON_FRAMEWORK)/Versions/A/_CodeSignature/CodeResources
# Headers and module map are sealed in the bundle, so they ship too, to keep the bundle valid.
DISTRIBUTABLES += $(wildcard $(SYPHON_FRAMEWORK)/Versions/A/Headers/*.h) $(SYPHON_FRAMEWORK)/Versions/A/Modules/module.modulemap
DISTRIBUTABLES += Frameworks/RLSyphon-LICENSE.txt
endif

# Video Player decoder: AVFoundation on macOS (src/video_player_decoder_mac.mm). Other hosts use
# src/video_player_decoder_stub.cpp, which the wildcard above already picked up. The stub is empty
# on macOS, so it is dropped from the build there. The decoder needs macOS 12 for its asynchronous
# track loader, which the Syphon block above already sets for the default Apple silicon build.
ifeq ($(shell uname -s),Darwin)
SOURCES := $(filter-out src/video_player_decoder_stub.cpp,$(SOURCES))
SOURCES += src/video_player_decoder_mac.mm
LDFLAGS += -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Foundation
endif

# Display recorder (the REC button): the same split. AVFoundation writes the movie on macOS
# (src/display_recorder_mac.mm, which links the frameworks added above). Other hosts use
# src/display_recorder_stub.cpp, which the wildcard above already picked up and which is empty on macOS.
ifeq ($(shell uname -s),Darwin)
SOURCES := $(filter-out src/display_recorder_stub.cpp,$(SOURCES))
SOURCES += src/display_recorder_mac.mm
endif

DISTRIBUTABLES += $(wildcard res/*)
DISTRIBUTABLES += docs/VIDEO-BUS.md include/vru_bus.h include/vru_bus_client.hpp include/LICENSE-MIT
DISTRIBUTABLES += presets/VideoOsc presets/DeflectionYoke presets/LineField presets/OneVoltCvOut presets/ScopeTrace presets/ColorLab presets/Keyer presets/BusMixer presets/WaveShaper presets/FreezeFrame presets/DualLfoChaos presets/Rotator presets/Shapes presets/VideoMixer presets/Focus

include $(RACK_DIR)/plugin.mk

# Offline checks for the video bus, renderer, Freeze Frame, Bus Mixer and slot allocators.
# Needs no Rack SDK. Proof PNGs go to build/bus_test_proofs (or pass a directory: make test TEST_PROOF_DIR=...).
TEST_SOURCES = tests/bus_test.cpp src/video_bus.cpp src/video_graph_renderer.cpp src/video_engine.cpp src/freeze_frame_store.cpp src/frame_ring.cpp src/video_external_frame.cpp src/signal_ring.cpp src/crt_beam.cpp
TEST_PROOF_DIR ?= build/bus_test_proofs

build/bus_test: $(TEST_SOURCES) $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc $(TEST_SOURCES) -pthread -o $@

# Public C interface (include/vru_bus.h). The in-process test compiles src/vru_bus_api.cpp with the
# renderer. The cross-dylib test builds a stand-alone VRU test library from the same sources, with
# hidden visibility except the one exported symbol, and loads it with dlopen from a program that
# includes only vru_bus.h. When a built plugin.dylib exists, the same program also loads it (with the SDK libRack.dylib on the library path), which is the check against the shipped binary.
API_CORE_SOURCES = src/vru_bus_api.cpp src/video_bus.cpp src/video_graph_renderer.cpp src/video_engine.cpp src/freeze_frame_store.cpp src/frame_ring.cpp src/video_external_frame.cpp src/signal_ring.cpp src/crt_beam.cpp
ifeq ($(shell uname -s),Darwin)
API_TEST_DYLIB = build/libvru_bus_testplugin.dylib
else
API_TEST_DYLIB = build/libvru_bus_testplugin.so
endif

build/vru_bus_api_test: tests/vru_bus_api_test.cpp $(API_CORE_SOURCES) include/vru_bus.h $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc -Iinclude tests/vru_bus_api_test.cpp $(API_CORE_SOURCES) -pthread -o $@

$(API_TEST_DYLIB): $(API_CORE_SOURCES) include/vru_bus.h $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -fPIC -fvisibility=hidden -shared -Isrc -Iinclude $(API_CORE_SOURCES) -pthread -o $@

build/vru_bus_dylib_test: tests/vru_bus_dylib_test.cpp include/vru_bus.h
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Iinclude tests/vru_bus_dylib_test.cpp -ldl -o $@

# Offline checks for Shapes (VRU-21): the distance picture, every control and mode, the culling
# against a brute-force reference, the presets, and the chain into the Keyer, Wave Shaper and
# Colorizer. Needs no Rack SDK. Proof PNGs go to the same directory as the bus test.
SHAPES_TEST_SOURCES = tests/shapes_test.cpp src/video_bus.cpp src/video_graph_renderer.cpp src/video_engine.cpp src/freeze_frame_store.cpp src/frame_ring.cpp src/video_external_frame.cpp src/signal_ring.cpp src/crt_beam.cpp

build/shapes_test: $(SHAPES_TEST_SOURCES) $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc $(SHAPES_TEST_SOURCES) -pthread -o $@

# Offline checks for Focus (VRU-24): each control against its tooltip, zero cost and zero change at rest, the
# bus node, and the contact sheet. Needs no Rack SDK. Run `build/focus_test <dir> bench` for timings.
FOCUS_TEST_SOURCES = tests/focus_test.cpp src/video_bus.cpp src/video_graph_renderer.cpp src/video_engine.cpp src/freeze_frame_store.cpp src/frame_ring.cpp src/video_external_frame.cpp src/signal_ring.cpp src/crt_beam.cpp

build/focus_test: $(FOCUS_TEST_SOURCES) $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc $(FOCUS_TEST_SOURCES) -pthread -o $@

# Offline checks for the Video Player core: source sorting, yt-dlp lookup and arguments,
# transport state machine, status messages and saved settings. Needs no Rack SDK.
VIDEO_PLAYER_TEST_SOURCES = tests/video_player_test.cpp src/video_player_core.cpp

build/video_player_test: $(VIDEO_PLAYER_TEST_SOURCES) src/video_player_core.hpp
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc $(VIDEO_PLAYER_TEST_SOURCES) -o $@

# Offline checks for the Video Player engine: worker thread, playback clock, play/pause/stop, loop
# wrap, end-of-clip count, YouTube download through a fake yt-dlp script, and saved state. A fake
# decoder replaces AVFoundation. Needs no Rack SDK, no yt-dlp, no video file and no network. Takes
# about 20 seconds. Scratch files go to build/video_player_engine_test_tmp.
VIDEO_PLAYER_ENGINE_TEST_SOURCES = tests/video_player_engine_test.cpp src/video_player_core.cpp src/video_player_engine.cpp src/video_external_frame.cpp

build/video_player_engine_test: $(VIDEO_PLAYER_ENGINE_TEST_SOURCES) $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc $(VIDEO_PLAYER_ENGINE_TEST_SOURCES) -pthread -o $@

# Smoke test for the Display module's SHOT and REC buttons with the real encoders (macOS only). It saves a
# PNG through the screenshot worker and records a .mov through the recorder, then reads both back with
# ImageIO and AVAssetReader and checks size, colours, orientation and length. It needs the Rack SDK only
# for the stb_image_write.h in its dep/include folder. Files go to build/display_capture_smoke_tmp and HOME
# points there, so the real home folder is never touched. Takes about 3 seconds.
DISPLAY_SMOKE = build/display_capture_smoke
DISPLAY_SMOKE_SOURCES = tests/display_capture_smoke.mm src/display_capture.cpp src/display_png.cpp src/display_recorder.cpp src/display_recorder_mac.mm

ifeq ($(shell uname -s),Darwin)
$(DISPLAY_SMOKE): $(DISPLAY_SMOKE_SOURCES) $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=12.0 -Isrc -I$(RACK_DIR)/dep/include \
		$(DISPLAY_SMOKE_SOURCES) -pthread \
		-framework AVFoundation -framework CoreMedia -framework CoreVideo -framework CoreGraphics -framework ImageIO -framework Foundation \
		-o $@
TEST_DISPLAY_DEP = $(DISPLAY_SMOKE)
TEST_DISPLAY_RUN = ./$(DISPLAY_SMOKE)
else
TEST_DISPLAY_DEP =
TEST_DISPLAY_RUN = @echo "Display capture smoke test skipped: needs macOS."
endif

# Render timing (median ms per frame): the DeflectionYoke (X-only and X+Y deflection), Scan Processor (lines x size)
# and the Scope Trace stroke.
BENCH_SOURCES = tests/deflection_yoke_bench.cpp src/video_bus.cpp src/video_graph_renderer.cpp src/video_engine.cpp src/freeze_frame_store.cpp src/frame_ring.cpp src/video_external_frame.cpp src/signal_ring.cpp src/crt_beam.cpp

build/deflection_yoke_bench: $(BENCH_SOURCES) $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc $(BENCH_SOURCES) -pthread -o $@

# Scan Processor render timing, one row per scanline count (same build rules, its own binary).
SCAN_PROCESSOR_BENCH_SOURCES = tests/scan_processor_bench.cpp src/video_bus.cpp src/video_graph_renderer.cpp src/video_engine.cpp src/freeze_frame_store.cpp src/frame_ring.cpp src/video_external_frame.cpp src/signal_ring.cpp src/crt_beam.cpp

build/scan_processor_bench: $(SCAN_PROCESSOR_BENCH_SOURCES) $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc $(SCAN_PROCESSOR_BENCH_SOURCES) -pthread -o $@

# Scope Trace stroke timing: depositScopeTrace at BEAM 0 and BEAM 1 over the longest TIME window, per mode and size.
SCOPE_BENCH_SOURCES = tests/scope_trace_bench.cpp src/video_bus.cpp src/video_graph_renderer.cpp src/video_engine.cpp src/freeze_frame_store.cpp src/frame_ring.cpp src/video_external_frame.cpp src/signal_ring.cpp src/crt_beam.cpp

build/scope_trace_bench: $(SCOPE_BENCH_SOURCES) $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc $(SCOPE_BENCH_SOURCES) -pthread -o $@

bench: build/deflection_yoke_bench build/scan_processor_bench build/scope_trace_bench
	./build/deflection_yoke_bench
	./build/scan_processor_bench
	./build/scope_trace_bench

# The vendored Syphon framework needs macOS 12. plugin.mk's compile.mk sets 10.9 for compiles,
# so the minimum is set here, after it, and applies to the whole plugin.
ifeq ($(RASTERLAB_SYPHON),1)
FLAGS += -mmacosx-version-min=12.0
# The bridge reads frames with the legacy GL calls that Apple marks deprecated; silence those.
FLAGS += -DGL_SILENCE_DEPRECATION
endif

# Round-trip test for the Syphon bridge: publishes a known frame through the output
# bridge and reads it back through the input bridge, in one process.
SYPHON_TEST = build/tests/syphon_roundtrip

ifeq ($(RASTERLAB_SYPHON),1)
$(SYPHON_TEST): tests/syphon_roundtrip.mm src/syphon_bridge_mac.mm src/syphon_bridge.hpp src/gl_video_texture.hpp
	@mkdir -p build/tests
	$(CXX) -std=c++17 -DRASTERLAB_WITH_SYPHON=1 -DGL_SILENCE_DEPRECATION -mmacosx-version-min=12.0 \
		-I src -F$(CURDIR)/Frameworks \
		tests/syphon_roundtrip.mm src/syphon_bridge_mac.mm \
		-framework Cocoa -framework OpenGL -framework RLSyphon \
		-Wl,-rpath,$(CURDIR)/Frameworks \
		-o $@
endif

# make test runs the bus tests, the Video Player core and engine tests, then the Syphon round-trip
# when the bridge is linked, then the Display capture smoke test on macOS.
ifeq ($(RASTERLAB_SYPHON),1)
TEST_SYPHON_DEP = $(SYPHON_TEST)
TEST_SYPHON_RUN = $(SYPHON_TEST)
else
TEST_SYPHON_DEP =
TEST_SYPHON_RUN = @echo "Syphon round-trip skipped: needs RASTERLAB_SYPHON=1 on macOS."
endif

build/octoscillator_test: tests/octoscillator_test.cpp src/octoscillator_math.hpp src/lorenz.hpp
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Isrc tests/octoscillator_test.cpp -o $@

# Headless integration with Rack's real Module/Port/JSON implementation (macOS SDK).
ifeq ($(shell uname -s),Darwin)
build/octoscillator_module_test: tests/octoscillator_module_test.cpp src/Octoscillator.cpp src/octoscillator_math.hpp src/lorenz.hpp src/panel_style.hpp
	mkdir -p build
	$(CXX) -std=c++17 -O2 -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -Isrc $< -L$(RACK_DIR) -lRack -o $@
TEST_OCT_MODULE_DEP = build/octoscillator_module_test
TEST_OCT_MODULE_RUN = DYLD_LIBRARY_PATH="$(abspath $(RACK_DIR))" ./build/octoscillator_module_test

SCOPE_MODULE_SOURCES = src/video_bus.cpp src/video_graph_renderer.cpp src/video_engine.cpp src/freeze_frame_store.cpp src/frame_ring.cpp src/video_external_frame.cpp src/signal_ring.cpp src/crt_beam.cpp
build/scope_trace_module_test: tests/scope_trace_module_test.cpp src/ScopeTrace.cpp $(SCOPE_MODULE_SOURCES) $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -Isrc tests/scope_trace_module_test.cpp $(SCOPE_MODULE_SOURCES) -L$(RACK_DIR) -lRack -o $@
TEST_SCOPE_MODULE_DEP = build/scope_trace_module_test
TEST_SCOPE_MODULE_RUN = DYLD_LIBRARY_PATH="$(abspath $(RACK_DIR))" ./build/scope_trace_module_test
endif

test: $(TEST_OCT_MODULE_DEP) $(TEST_SCOPE_MODULE_DEP) build/octoscillator_test build/bus_test build/shapes_test build/focus_test build/demo_patch_render_test build/video_player_test build/video_player_engine_test build/vru_bus_api_test build/vru_bus_dylib_test $(API_TEST_DYLIB) $(TEST_SYPHON_DEP) $(TEST_DISPLAY_DEP)
	./build/octoscillator_test
	$(TEST_OCT_MODULE_RUN)
	$(TEST_SCOPE_MODULE_RUN)
	./build/bus_test $(TEST_PROOF_DIR)
	./build/shapes_test $(TEST_PROOF_DIR)
	./build/focus_test $(TEST_PROOF_DIR)
	./build/demo_patch_render_test
	./build/video_player_test
	./build/video_player_engine_test
	./build/vru_bus_api_test
	./build/vru_bus_dylib_test $(API_TEST_DYLIB)
	@if [ -f plugin.dylib ]; then DYLD_LIBRARY_PATH="$(abspath $(RACK_DIR))" ./build/vru_bus_dylib_test ./plugin.dylib; fi
	$(TEST_SYPHON_RUN)
	$(TEST_DISPLAY_RUN)

# Picture check for the numbered demo patches: every LFO-modulated destination at the ends of its swing must
# leave a lit, centred picture. No Rack needed. See scripts/check-demo-ranges.py for the numeric table.
DEMO_RENDER_TEST_SOURCES = tests/demo_patch_render_test.cpp src/video_bus.cpp src/video_graph_renderer.cpp src/video_engine.cpp src/freeze_frame_store.cpp src/frame_ring.cpp src/video_external_frame.cpp src/signal_ring.cpp src/crt_beam.cpp

build/demo_patch_render_test: $(DEMO_RENDER_TEST_SOURCES) $(wildcard src/*.hpp)
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc $(DEMO_RENDER_TEST_SOURCES) -pthread -o $@

# Demo patches and the demo clip are NOT part of the plugin package. This zip is a separate
# download for the GitHub release: the numbered demo patches, plus the Muybridge clip and its credits.
# The clip itself is not in the source repository: it ships in the release download only. To run this target
# yourself, put muybridge-horse-in-motion.mp4 (see release-extras/CREDITS.md for the source) in release-extras/.
RELEASE_EXTRAS_ZIP = dist/VideoResearchUnit-demos-$(VERSION).zip
release-extras:
	mkdir -p dist
	rm -f $(RELEASE_EXTRAS_ZIP)
	rm -rf build/release-extras && mkdir -p "build/release-extras/VideoResearchUnit demos/clip"
	cp patches/[0-9]*.vcv "build/release-extras/VideoResearchUnit demos/"
	cp release-extras/muybridge-horse-in-motion.mp4 release-extras/CREDITS.md "build/release-extras/VideoResearchUnit demos/clip/"
	cp release-extras/README-demo-clip.txt "build/release-extras/VideoResearchUnit demos/README.txt"
	cd build/release-extras && zip -q -X -r ../../$(RELEASE_EXTRAS_ZIP) "VideoResearchUnit demos"

.PHONY: test bench release-extras
