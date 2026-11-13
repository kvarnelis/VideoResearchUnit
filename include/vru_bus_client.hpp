// vru_bus_client.hpp - header-only helper for Rack plugins that want to use the VRU video bus.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kazys Varnelis
// Licence text: LICENSE-MIT, next to this file. The rest of VRU is GPL-3.0-or-later.
// Needs the Rack SDK headers and vru_bus.h.
//
//   const VruBusApiV1* vru = vruBusConnect();   // null when VRU is not installed or too old
//   if (vru) { int id = vru->busIdFromVoltage(v); ... }
//
// Call it when you need it (for example when a module is created, or once per draw) and check for
// null every time. Do not keep the pointer past a point where VRU might be unloaded.
#pragma once

#include "vru_bus.h"

#include <rack.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

inline const VruBusApiV1* vruBusConnect(uint32_t version = VRU_BUS_API_VERSION) {
	rack::plugin::Plugin* plugin = rack::plugin::getPlugin(VRU_BUS_PLUGIN_SLUG);
	if (!plugin || !plugin->handle) {
		return nullptr;
	}
#if defined(_WIN32)
	void* symbol = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(plugin->handle), VRU_BUS_GET_API_SYMBOL));
#else
	void* symbol = dlsym(plugin->handle, VRU_BUS_GET_API_SYMBOL);
#endif
	if (!symbol) {
		return nullptr;
	}
	const VruBusApiV1* api = reinterpret_cast<VruBusGetApiFn>(symbol)(version);
	if (!api || api->version < version || api->structSize < sizeof(VruBusApiV1)) {
		return nullptr;
	}
	return api;
}
