#include "plugin.hpp"
#include "shapes_gpu.hpp"
#include "display_capture.hpp"
#include "display_picture_size.hpp"
#include "render_service.hpp"
#include <cstring>

#include <exception>
#include <string>

Plugin* pluginInstance;

namespace {
// The render service's tick source: Rack's time stamp for the UI frame being drawn. Every draw in one UI frame sees
// the same value and the next frame sees another, so the service draws a bus once per frame however many modules ask.
// This is the one place a tick is defined.
uint64_t uiFrameTick() {
	const double frameTime = APP && APP->window ? APP->window->getFrameTime() : 0.0;
	uint64_t bits = 0;
	static_assert(sizeof(bits) == sizeof(frameTime), "a double is eight bytes");
	std::memcpy(&bits, &frameTime, sizeof(bits));
	return bits;
}

// Creates a folder for the Display module's screenshots and videos, with its parents. Rack's
// createDirectories() returns false when the folder is already there and throws when it cannot make
// it, so the answer is whether the folder exists afterwards. Runs on the Display's worker threads.
bool createCaptureFolder(const std::string& path, std::string& error) {
	try {
		system::createDirectories(path);
		if (system::isDirectory(path)) {
			return true;
		}
		error = "could not create the folder " + path;
	}
	catch (const std::exception& e) {
		error = e.what();
		try {
			if (system::isDirectory(path)) {
				return true;
			}
		}
		catch (const std::exception&) {
		}
	}
	return false;
}
}

void init(Plugin* p) {
	pluginInstance = p;
	display_capture::setCreateFolderHook(createCaptureFolder);
	renderService().setTickSource(uiFrameTick);
	// Catalogue order, VRU–01 to VRU–28, the same order as plugin.json.
	p->addModel(modelDeflectionYoke);
	p->addModel(modelScanProcessor);
	p->addModel(modelScopeTrace);
	p->addModel(modelLineField);
	p->addModel(modelVideoOsc);
	p->addModel(modelRamps);
	p->addModel(modelVideoPlayer);
	p->addModel(modelSyphonIn);
	p->addModel(modelColorLab);
	p->addModel(modelColorizer);
	p->addModel(modelWaveShaper);
	p->addModel(modelKeyer);
	p->addModel(modelBusMixer);
	p->addModel(modelFeedback);
	p->addModel(modelFreezeFrame);
	p->addModel(modelDisplay);
	p->addModel(modelSyphonOut);
	p->addModel(modelDualLfoChaos);
	p->addModel(modelOneVoltCvOut);
	p->addModel(modelRotator);
	p->addModel(modelFocus);
	p->addModel(modelShapes);
	p->addModel(modelVideoMixer);
	p->addModel(modelOctoscillator);
	p->addModel(modelVectorOsc);
	p->addModel(modelVideoProbe);
	p->addModel(modelFrameDelay);
	p->addModel(modelScanSync);
	p->addModel(modelRaster);
}

// Plugin-wide settings, stored by Rack in settings.json under settings["pluginSettings"]["VideoResearchUnit"].
// "captureFolder" is the folder last picked for Display screenshots and videos, so new Display modules in
// any patch start with it, across Rack restarts. "" means the default folder.
// "keepRunningHidden" is the Display menu item "Keep video running when hidden" (true when missing).
extern "C" json_t* settingsToJson() {
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "preferredPictureLines", json_integer(displayPreferredLines.load()));
	json_object_set_new(rootJ, "captureFolder", json_string(display_capture::lastPickedFolder().c_str()));
	json_object_set_new(rootJ, "keepRunningHidden", json_boolean(renderService().hiddenRendering()));
	json_object_set_new(rootJ, "renderOnOwnThread", json_boolean(renderService().threadMode()));
	json_object_set_new(rootJ, "fullPrecision", json_boolean(renderService().fullPrecision()));
	json_object_set_new(rootJ, "shapesOnGraphicsCard", json_boolean(shapes_gpu::enabled()));
	return rootJ;
}

extern "C" void settingsFromJson(json_t* rootJ) {
	if (!rootJ) {
		return;
	}
	displayPreferredLines.store(displayClampLines(json_integer_value(json_object_get(rootJ, "preferredPictureLines")), kDisplayDefaultLines));
	json_t* keepJ = json_object_get(rootJ, "keepRunningHidden");
	if (json_is_boolean(keepJ)) {
		renderService().setHiddenRendering(json_is_true(keepJ));
	}
	// "renderOnOwnThread" is the Display menu item "Render video on its own thread" (false when missing).
	json_t* threadJ = json_object_get(rootJ, "renderOnOwnThread");
	if (json_is_boolean(threadJ)) {
		renderService().setThreadMode(json_is_true(threadJ));
	}
	// "fullPrecision" is the Display menu item "Full precision between modules" (false when missing).
	json_t* precisionJ = json_object_get(rootJ, "fullPrecision");
	if (json_is_boolean(precisionJ)) {
		renderService().setFullPrecision(json_is_true(precisionJ));
	}
	json_t* gpu = json_object_get(rootJ, "shapesOnGraphicsCard");
	if (json_is_boolean(gpu)) shapes_gpu::setEnabled(json_is_true(gpu));
	json_t* folderJ = json_object_get(rootJ, "captureFolder");
	if (json_is_string(folderJ)) {
		display_capture::setLastPickedFolder(json_string_value(folderJ));
	}
}
