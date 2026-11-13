#pragma once

#include "video_engine.hpp"

// Video bus model (see docs/architecture.md, "Video bus model"):
//
// - Every module instance that has a video output owns one bus id in 1..kVideoBusCount,
//   claimed with VideoBusOwner when the module is constructed and released when it is destroyed.
// - The output cable carries that id as its voltage (id 1 = 1 V, id 2 = 2 V, ...). Consumers
//   convert the voltage back with videoBusIdFromVoltage(). A voltage that is not a bus id
//   (0 V, or above the bus count) resolves to 0, which means "no bus".
// - Each bus holds one graph node. The owning module publishes the node from the audio thread
//   once per sample. Readers on the UI thread take a consistent copy of the whole node
//   (seqlock). One writer per bus id.
// - Unpatched processor inputs read bus 1 (legacy default). Unpatched Bus Mixer B and MAT
//   inputs mean "absent" and are not bus 1.

constexpr int kVideoBusCount = 64;

enum class VideoBusNodeType : int {
	Empty = 0,
	Generator = 1,
	DeflectionYoke = 2,
	LineField = 3,
	ExternalFrame = 4,
	ScopeTrace = 5,
	ColorLab = 6,
	Keyer = 7,
	BusMixer = 8,
	WaveShaper = 9,
	FreezeFrame = 10,
	ScanProcessor = 11,
	Colorizer = 12,
	Ramps = 13,
	Feedback = 14,
	Rotator = 15,
	Shapes = 16,
	VideoMixer = 17,
};

// Plain-old-data node stored in a bus. Must stay trivially copyable: it is copied word by word
// through the seqlock.
struct VideoBusNode {
	VideoBusNodeType type = VideoBusNodeType::Empty;
	VideoParams generator;
	int sourceBusId = 0;
	DeflectionYokeParams deflectionYoke;
	LineFieldParams lineField;
	int externalFrameId = 0;
	int scopeTraceSlotId = 0;
	ScopeTraceParams scopeTrace;
	ColorLabParams colorLab;
	KeyerParams keyer;
	BusMixerParams busMixer;
	WaveShaperParams waveShaper;
	FreezeFrameParams freezeFrame;
	ScanProcessorParams scanProcessor;
	ColorizerParams colorizer;
	RampsParams ramps;
	FeedbackParams feedback;
	RotatorParams rotator;
	ShapesParams shapes;
	VideoMixerParams videoMixer;
};

// Publishing (audio thread, owner of busId only). Each call replaces the whole node.
void videoBusPublish(int busId, const VideoParams& params);
void videoBusPublishDeflectionYoke(int busId, int sourceBusId, const DeflectionYokeParams& params);
void videoBusPublishLineField(int busId, int sourceBusId, const LineFieldParams& params);
void videoBusPublishExternalFrame(int busId, int externalFrameId);
void videoBusPublishScopeTrace(int busId, int scopeTraceSlotId, const ScopeTraceParams& params);
void videoBusPublishColorLab(int busId, int sourceBusId, const ColorLabParams& params);
void videoBusPublishKeyer(int busId, int sourceBusId, const KeyerParams& params);
// busB or matteBus of 0 means "absent".
void videoBusPublishBusMixer(int busId, int sourceBusId, const BusMixerParams& params);
void videoBusPublishWaveShaper(int busId, int sourceBusId, const WaveShaperParams& params);
void videoBusPublishFreezeFrame(int busId, int sourceBusId, const FreezeFrameParams& params);
void videoBusPublishScanProcessor(int busId, int sourceBusId, const ScanProcessorParams& params);
void videoBusClear(int busId);

// Reading (any thread). Returns a consistent snapshot. False when the bus is empty.
bool videoBusRead(int busId, VideoParams& params);
bool videoBusReadNode(int busId, VideoBusNode& node);
bool videoBusIsActive(int busId);

// Bus id allocation (UI/construction thread only).
// Claims preferredId when free, else the lowest free id. Returns 0 when all buses are in use.
int videoBusAllocate(int preferredId = 0);
// Clears the bus and makes its id available again.
void videoBusRelease(int busId);

// Voltage <-> bus id. Returns 0 when the voltage is not a valid bus id.
int videoBusIdFromVoltage(float voltage);

// Voltage a pass-through (THRU) output carries: the input's token when patched, 0 V (no bus) when not.
inline float videoThruVoltage(bool connected, float inputVoltage) {
	return connected ? inputVoltage : 0.f;
}

// Owns the bus id of one module instance for the lifetime of the module.
class VideoBusOwner {
public:
	VideoBusOwner();
	~VideoBusOwner();
	VideoBusOwner(const VideoBusOwner&) = delete;
	VideoBusOwner& operator=(const VideoBusOwner&) = delete;

	int id() const {
		return busId;
	}
	float voltage() const {
		return static_cast<float>(busId);
	}
	// Used on patch load. Takes preferredId when it is free; otherwise keeps a fresh free id.
	void claimPreferred(int preferredId);

private:
	int busId = 0;
};

void videoBusPublishColorizer(int busId, int sourceBusId, const ColorizerParams& params);
void videoBusPublishRamps(int busId, const RampsParams& params);
void videoBusPublishFeedback(int busId, int sourceBusId, const FeedbackParams& params);
void videoBusPublishRotator(int busId, int sourceBusId, const RotatorParams& params);
void videoBusPublishShapes(int busId, const ShapesParams& params);
// The Video Mixer reads its four pictures, pattern, key and matte buses from the params (0 = absent).
void videoBusPublishVideoMixer(int busId, const VideoMixerParams& params);
