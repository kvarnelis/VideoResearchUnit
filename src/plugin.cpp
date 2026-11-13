#include "plugin.hpp"
#include "display_capture.hpp"

#include <exception>
#include <string>

Plugin* pluginInstance;

namespace {
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
	// Catalogue order, VRU–01 to VRU–23, the same order as plugin.json.
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
	p->addModel(modelShapes);
	p->addModel(modelVideoMixer);
	p->addModel(modelOctoscillator);
}

// Plugin-wide settings, stored by Rack in settings.json under settings["pluginSettings"]["VideoResearchUnit"].
// The only one is the folder last picked for Display screenshots and videos, so new Display modules in
// any patch start with it, across Rack restarts. "" means the default folder.
extern "C" json_t* settingsToJson() {
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "captureFolder", json_string(display_capture::lastPickedFolder().c_str()));
	return rootJ;
}

extern "C" void settingsFromJson(json_t* rootJ) {
	if (!rootJ) {
		return;
	}
	json_t* folderJ = json_object_get(rootJ, "captureFolder");
	if (json_is_string(folderJ)) {
		display_capture::setLastPickedFolder(json_string_value(folderJ));
	}
}
