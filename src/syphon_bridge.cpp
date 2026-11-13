#include "syphon_bridge.hpp"

#include <memory>

#ifndef RASTERLAB_WITH_SYPHON
std::unique_ptr<SyphonOutputBridge> createSyphonOutputBridge() {
	return std::make_unique<NullSyphonOutputBridge>();
}

std::unique_ptr<SyphonInputBridge> createSyphonInputBridge() {
	return std::make_unique<NullSyphonInputBridge>();
}
#endif
