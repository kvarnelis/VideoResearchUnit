/*
 * vru_bus.h - public C interface to the Video Research Unit (VRU) video bus.
 *
 * Plain C (C++ compatible). Only fixed-width integers, floats, size_t and pointers cross the boundary.
 * No Rack, C++ or STL types. You do not link against VRU: you find it at run time (see
 * vru_bus_client.hpp, or do the dlsym / GetProcAddress call yourself) and call through the table.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kazys Varnelis
 * Licence text: LICENSE-MIT, next to this file. The rest of VRU is GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------------------------
 * Finding the interface
 *
 *   rack::plugin::getPlugin("VideoResearchUnit")->handle   is the OS library handle of VRU's plugin library.
 *   dlsym(handle, "vruBusGetApi")                  (mac/linux) or GetProcAddress (Windows).
 *
 * vruBusGetApi(requestedVersion) returns a pointer to a table, or NULL when VRU does not offer that
 * version. The table lives as long as VRU stays loaded; do not free it.
 *
 * ---------------------------------------------------------------------------------------------
 * Bus ids
 *
 * A BUS cable carries an integer bus id (1..VRU_BUS_ID_MAX) as its voltage. 0 means "no bus".
 * A source module puts the id it got from claimSource on its output as a voltage (id 3 = 3.0 V),
 * exactly as VRU's own sources do. Do not store an id in patch data: ids are handed out per session.
 *
 * ---------------------------------------------------------------------------------------------
 * Threads
 *
 *   busIdFromVoltage, isActive   any thread, lock free, cheap. Safe on the audio thread.
 *   claimSource, releaseSource   UI / module construction and destruction thread. May allocate or lock.
 *                                NOT for the audio thread.
 *   publishFrame                 Any thread EXCEPT the audio thread: it allocates (first call and
 *                                whenever the size changes), copies the whole picture and takes a short
 *                                internal lock that the renderer also takes. Call it from your UI thread
 *                                or a worker thread of your own. One thread at a time per source id.
 *   readFrame                    Calls are serialised internally, so any one thread at a time works, but
 *                                it is a full CPU render of the whole chain behind the bus. Call it
 *                                from the UI thread (as VRU's Display does, once per draw) or a worker
 *                                thread. NOT from the audio thread.
 *
 * Versions
 *
 * The table starts with { version, structSize }. New versions only append function pointers, so a
 * client written for version 1 keeps working against a newer table. Check structSize before using any
 * member that lies beyond the size you were compiled against.
 */
#ifndef VRU_BUS_H
#define VRU_BUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRU_BUS_API_VERSION 1u
#define VRU_BUS_PLUGIN_SLUG "VideoResearchUnit"
#define VRU_BUS_GET_API_SYMBOL "vruBusGetApi"

/* Bus ids run 1..VRU_BUS_ID_MAX. 0 is "no bus". */
#define VRU_BUS_ID_MAX 64
/* Largest picture width or height accepted by readFrame and publishFrame. */
#define VRU_BUS_MAX_DIMENSION 4096

/* Status codes (readFrame, publishFrame). 0 is success, errors are negative. */
#define VRU_BUS_OK 0
#define VRU_BUS_ERR_BAD_ARGUMENT (-1) /* null pointer, size out of range, stride too small */
#define VRU_BUS_ERR_BAD_ID (-2)       /* id not in 1..VRU_BUS_ID_MAX, or not a source you claimed */
#define VRU_BUS_ERR_NO_SIGNAL (-3)    /* the bus is empty, or its chain cannot be drawn (cycle) */

typedef struct VruBusApiV1 {
	uint32_t version;    /* VRU_BUS_API_VERSION this table implements */
	uint32_t structSize; /* sizeof of the table VRU filled in */

	/* Same rule as VRU's own receivers: rounds to nearest, returns 1..64, else 0. NaN gives 0. */
	int (*busIdFromVoltage)(float voltage);

	/* 1 when some module currently publishes a picture on this bus, else 0. */
	int (*isActive)(int busId);

	/* Renders the picture behind busId at width x height (each 1..VRU_BUS_MAX_DIMENSION) into rgba:
	 * 8 bits per channel R,G,B,A, top row first, strideBytes between rows (>= width * 4). The buffer
	 * must hold height * strideBytes bytes (the last row needs only width * 4). Returns VRU_BUS_OK,
	 * or an error code and leaves the buffer untouched. */
	int (*readFrame)(int busId, int width, int height, uint8_t* rgba, size_t strideBytes);

	/* Claims a bus id for an external picture source. Returns the id (1..64), or 0 when none is
	 * free. The id carries no picture until the first publishFrame. Put it on your output cable as
	 * a voltage. Release it when your module is deleted. */
	int (*claimSource)(void);

	/* Copies a width x height RGBA picture (strideBytes between rows) into storage VRU owns. VRU
	 * scales it to whatever size each sink renders. busId must be one you claimed. */
	int (*publishFrame)(int busId, const uint8_t* rgba, int width, int height, size_t strideBytes);

	/* Gives the id back and frees the picture. Ignores ids you did not claim. */
	void (*releaseSource)(int busId);
} VruBusApiV1;

/* Signature of the one exported symbol. Returns NULL for a version VRU does not offer. */
typedef const VruBusApiV1* (*VruBusGetApiFn)(uint32_t requestedVersion);

#ifdef __cplusplus
}
#endif

#endif /* VRU_BUS_H */
