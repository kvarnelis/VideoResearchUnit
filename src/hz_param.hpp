#pragma once

#include "plugin.hpp"
#include "expo_hz.hpp"

#include <cstdio>
#include <string>

// Knob whose stored value is 0..1 and whose displayed value is exponential Hz.
struct ExpHzParamQuantity : engine::ParamQuantity {
	float minHz = 0.1f;
	float maxHz = 8000.f;

	float getDisplayValue() override {
		return expHzFromNormalized(getValue(), minHz, maxHz);
	}

	void setDisplayValue(float displayValue) override {
		setValue(normalizedFromExpHz(displayValue, minHz, maxHz));
	}

	std::string getDisplayValueString() override {
		char text[32];
		const float hz = getDisplayValue();
		if (hz >= 1000.f) {
			std::snprintf(text, sizeof(text), "%.2f kHz", hz / 1000.f);
		}
		else if (hz >= 10.f) {
			std::snprintf(text, sizeof(text), "%.1f Hz", hz);
		}
		else {
			std::snprintf(text, sizeof(text), "%.2f Hz", hz);
		}
		return text;
	}

	std::string getUnit() override {
		return " Hz";
	}
};
