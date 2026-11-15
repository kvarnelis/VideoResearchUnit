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

/* ---------------------------------------------------------------------------------------------
 * Version 2
 *
 * Version 2 is version 1 plus appended entries. Nothing in version 1 changed: vruBusGetApi(1) still
 * returns the same version 1 table. vruBusGetApi(2) returns a VruBusApiV2, whose first member is the
 * version 1 table laid out exactly as before (same pointers in the same order), followed by the new
 * entries. A table's version field says which table you hold: 1 for the version 1 table, 2 for this one.
 * The function returns the address as a const VruBusApiV1* (its declared type has not changed); cast it to
 * const VruBusApiV2* when you asked for version 2.
 *
 * Status codes. Errors are negative, as in version 1. Version 2 adds two non-negative codes that are not
 * errors: test `status < 0` for failure. Version 1 entries never return them.
 *
 * Float pictures. 4 floats per pixel (R, G, B, A), top row first, stride measured in FLOATS, not bytes.
 * The unit is 0..1: 0.0 is the darkest level and 1.0 the brightest, there is no gamma and no premultiplication.
 * Width, height, bus id and status codes follow readFrame. What a float value is depends on VRU's plugin
 * setting "Full precision between modules":
 *   - Setting off (the default): a value is exactly byte / 255.0f, the readFrame picture divided by 255, so
 *     there are 256 levels per channel.
 *   - Setting on: a value is the picture before it was rounded to 8 bits, divided by 255.0f, so it can fall
 *     between two levels (a chain of gain changes keeps its fine steps). The byte read of the same picture is
 *     this float picture multiplied by 255 and rounded half up. This holds once VRU has seen a float read of
 *     that bus at that size: the first float read after a cold start, or after you stopped reading for 120
 *     ticks, may be byte / 255.0f, and the next tick's read is at full precision. A bus whose
 *     picture is 8-bit anyway (a video file, Syphon In, a picture you published) reads byte / 255.0f in
 *     both settings.
 * The 8-bit reads (readFrame, readFrameEx) are the rounded picture in both settings and do not change.
 *
 * The tick. Every Ex read reports a tick: an opaque 64-bit counter that VRU's render service keeps. It
 * never goes down while VRU stays loaded and it has no promised rate: it may rise 60 times a second, or
 * more slowly, or in jumps, and nothing may be inferred from the difference of two ticks except that they
 * differ. It is not a clock and not a frame count. Two reads that see the same tick are in the same
 * service tick. Pass the tick a read returned as sinceTick on the next read of the same bus at the same
 * size. 0 is never a tick, so sinceTick 0 means "no earlier read".
 *
 * Threads. readFrameF32, readFrameEx, readFrameF32Ex and getNativeSize follow the rule for readFrame: one
 * thread at a time works, from the UI thread or a worker thread of your own, and NEVER from the audio
 * thread, which means never from a module's process(). The reads take the same internal lock as readFrame.
 */
#define VRU_BUS_API_VERSION_2 2u

/* Non-error status codes (version 2). */
#define VRU_BUS_NO_NATIVE_SIZE 1 /* getNativeSize: this bus has no native size (a generated picture) */
#define VRU_BUS_UNCHANGED 2      /* readFrameEx / readFrameF32Ex: nothing new since sinceTick; buffer untouched */

/* VruBusReadInfo.flags */
#define VRU_BUS_READ_COPY_ALWAYS 1u /* copy the picture even when it is not new; isNew still tells */

/* In and out parameters of the Ex reads. Set structSize to sizeof(VruBusReadInfo) before the call; a
 * smaller value is VRU_BUS_ERR_BAD_ARGUMENT. Later versions may append fields and raise the size. */
typedef struct VruBusReadInfo {
	uint32_t structSize; /* in: sizeof(VruBusReadInfo) */
	uint32_t flags;      /* in: 0, or VRU_BUS_READ_COPY_ALWAYS */
	uint64_t sinceTick;  /* in: the tick of your last read of this bus at this size, or 0 */
	uint64_t tick;       /* out: the service tick the picture belongs to (0 when the read failed) */
	int32_t isNew;       /* out: 1 when the picture is newer than sinceTick, else 0 */
	int32_t reserved;    /* out: 0 */
} VruBusReadInfo;

/* Out parameter of getNativeSize. Set structSize to sizeof(VruBusNativeSize) before the call. */
typedef struct VruBusNativeSize {
	uint32_t structSize; /* in: sizeof(VruBusNativeSize) */
	int32_t width;       /* out: the source's own width in pixels, 0 when there is none */
	int32_t height;      /* out: the source's own height in pixels, 0 when there is none */
	int32_t reserved;    /* out: 0 */
} VruBusNativeSize;

typedef struct VruBusApiV2 {
	/* The whole version 1 table, unchanged in layout. Only its version (2) and structSize
	 * (sizeof(VruBusApiV2)) differ from the table vruBusGetApi(1) returns. */
	VruBusApiV1 v1;

	/* Like readFrame, but into float RGBA: 4 floats per pixel, 0..1, strideFloats floats between rows
	 * (>= width * 4). The buffer must hold (height - 1) * strideFloats + width * 4 floats. Same status
	 * codes and limits as readFrame; an error leaves the buffer untouched. With VRU's setting "Full
	 * precision between modules" off, a value is byte / 255.0f; with it on, it is the picture before
	 * rounding / 255.0f (see "Float pictures" above for the first-read rule). */
	int (*readFrameF32)(int busId, int width, int height, float* rgba, size_t strideFloats);

	/* readFrame with the tick. info must not be null. With sinceTick set and no new service tick since
	 * it, nothing is drawn or copied and the result is VRU_BUS_UNCHANGED (info->isNew is 0), unless
	 * VRU_BUS_READ_COPY_ALWAYS is set. Otherwise the same as readFrame, and info->tick and info->isNew are
	 * filled in. On an error info->tick is 0 and the buffer is untouched. */
	int (*readFrameEx)(int busId, int width, int height, uint8_t* rgba, size_t strideBytes, VruBusReadInfo* info);

	/* The float read with the tick, with the rule of readFrameEx. */
	int (*readFrameF32Ex)(int busId, int width, int height, float* rgba, size_t strideFloats, VruBusReadInfo* info);

	/* The size of the picture a source module publishes on this bus, for sources that bring their own
	 * pictures: Video Player, Syphon In and pictures published through publishFrame. Returns VRU_BUS_OK and
	 * fills out. A bus whose picture is generated or processed (an oscillator, Ramps, Shapes, Scope Trace,
	 * or any module that takes a bus in) has no native size: the result is VRU_BUS_NO_NATIVE_SIZE with
	 * width and height 0. VRU_BUS_ERR_BAD_ID, VRU_BUS_ERR_BAD_ARGUMENT (null out, small structSize) and
	 * VRU_BUS_ERR_NO_SIGNAL (empty bus, or a source that has not published a picture yet) as for readFrame. */
	int (*getNativeSize)(int busId, VruBusNativeSize* out);
} VruBusApiV2;

#ifdef __cplusplus
}
#endif

#endif /* VRU_BUS_H */
