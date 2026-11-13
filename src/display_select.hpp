#pragma once

#include "video_bus.hpp"

// Display selection rules, shared by the module and the offline tests. No Rack dependencies.
inline int displayClampInput(int input) {
	return input >= 0 && input < 3 ? input : 0;
}

inline int displayNextInput(int current) {
	return current >= 0 && current < 2 ? current + 1 : 0;
}

inline int displayInputBusId(int input, bool connected, float voltage) {
	return connected ? videoBusIdFromVoltage(voltage) : (input == 0 ? 1 : 0);
}
