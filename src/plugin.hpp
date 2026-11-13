#pragma once

#include <rack.hpp>

#include "video_bus.hpp"

using namespace rack;

// Bus id persistence. Saves "busId". Loads "busId" when present, otherwise the legacy
// voltage stored under legacyVoltageKey (old patches). Collisions are resolved by the owner.
inline void saveVideoBusId(json_t* rootJ, const VideoBusOwner& videoBus) {
	json_object_set_new(rootJ, "busId", json_integer(videoBus.id()));
}

inline void loadVideoBusId(json_t* rootJ, VideoBusOwner& videoBus, const char* legacyVoltageKey) {
	if (json_t* idJ = json_object_get(rootJ, "busId")) {
		if (json_is_integer(idJ)) {
			videoBus.claimPreferred(static_cast<int>(json_integer_value(idJ)));
		}
		return;
	}
	if (json_t* voltageJ = json_object_get(rootJ, legacyVoltageKey)) {
		if (json_is_number(voltageJ)) {
			videoBus.claimPreferred(videoBusIdFromVoltage(static_cast<float>(json_number_value(voltageJ))));
		}
	}
}

extern Plugin* pluginInstance;
extern Model* modelVideoOsc;
extern Model* modelDisplay;
extern Model* modelDeflectionYoke;
extern Model* modelLineField;
extern Model* modelOneVoltCvOut;
extern Model* modelScopeTrace;
extern Model* modelSyphonIn;
extern Model* modelSyphonOut;
extern Model* modelVideoPlayer;

extern Model* modelColorLab;
extern Model* modelKeyer;
extern Model* modelBusMixer;
extern Model* modelWaveShaper;
extern Model* modelFreezeFrame;
extern Model* modelDualLfoChaos;
extern Model* modelScanProcessor;
extern Model* modelColorizer;
extern Model* modelRamps;
extern Model* modelFeedback;
extern Model* modelRotator;
extern Model* modelShapes;
extern Model* modelVideoMixer;

extern Model* modelOctoscillator;
