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

// ---------------------------------------------------------------------------------------------
// Version 2. vruBusConnect() above is unchanged and still returns the version 1 table.
//
//   VruBusClient vru = vruBusConnectBest();   // asks for version 2, falls back to version 1
//   if (vru.v1) {                             // VRU is installed (any version that serves 1 or 2)
//       if (vru.hasFloatRead()) { vru.v2->readFrameF32(id, w, h, floats, size_t(w) * 4); }
//       else                    { vru.v1->readFrame(id, w, h, bytes, size_t(w) * 4); }
//   }
//
// vruBusConnectBest() never throws and never allocates. v1 is non-null whenever VRU answered with either
// version; v2 is non-null only when the installed VRU serves version 2 (the table's v1 member is then the
// same table as v1). A plugin older than 2.4 serves only version 1: v2 is null and every version 2 entry
// is absent. Check hasFloatRead(), hasTickRead() and hasNativeSize() before calling an entry.
struct VruBusClient {
	const VruBusApiV1* v1 = nullptr;
	const VruBusApiV2* v2 = nullptr;

	bool connected() const {
		return v1 != nullptr;
	}
	bool hasVersion2() const {
		return v2 != nullptr;
	}
	bool hasFloatRead() const {
		return v2 != nullptr && v2->readFrameF32 != nullptr;
	}
	bool hasTickRead() const {
		return v2 != nullptr && v2->readFrameEx != nullptr && v2->readFrameF32Ex != nullptr;
	}
	bool hasNativeSize() const {
		return v2 != nullptr && v2->getNativeSize != nullptr;
	}
};

// Picks the best table a vruBusGetApi function offers: version 2, else version 1, else nothing. The lookup of
// the function is separate so that it can be tested with a stand-in that only knows version 1 (an older VRU).
inline VruBusClient vruBusConnectFrom(VruBusGetApiFn getApi) {
	VruBusClient client;
	if (!getApi) {
		return client;
	}
	const VruBusApiV1* second = getApi(VRU_BUS_API_VERSION_2);
	if (second && second->version >= VRU_BUS_API_VERSION_2 && second->structSize >= sizeof(VruBusApiV2)) {
		client.v2 = reinterpret_cast<const VruBusApiV2*>(second);
		client.v1 = &client.v2->v1;
		return client;
	}
	const VruBusApiV1* first = getApi(VRU_BUS_API_VERSION);
	if (first && first->version >= VRU_BUS_API_VERSION && first->structSize >= sizeof(VruBusApiV1)) {
		client.v1 = first;
	}
	return client;
}

// Asks the installed VRU for version 2 and falls back to version 1. Null members when VRU is not installed.
inline VruBusClient vruBusConnectBest() {
	rack::plugin::Plugin* plugin = rack::plugin::getPlugin(VRU_BUS_PLUGIN_SLUG);
	if (!plugin || !plugin->handle) {
		return VruBusClient();
	}
#if defined(_WIN32)
	void* symbol = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(plugin->handle), VRU_BUS_GET_API_SYMBOL));
#else
	void* symbol = dlsym(plugin->handle, VRU_BUS_GET_API_SYMBOL);
#endif
	return vruBusConnectFrom(reinterpret_cast<VruBusGetApiFn>(symbol));
}
