#include "video_bus.hpp"
#include "video_slot_allocator.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace {
static_assert(std::is_trivially_copyable<VideoBusNode>::value, "VideoBusNode is copied through the seqlock");

constexpr size_t kPayloadWords = (sizeof(VideoBusNode) + sizeof(uint64_t) - 1) / sizeof(uint64_t);
constexpr int kMaxReadAttempts = 256;

// Seqlock: the writer makes sequence odd, writes the payload, then makes it even again.
// A reader retries when it sees an odd sequence or a sequence that changed while it copied.
// Payload words are atomics accessed with relaxed ordering, so there is no data race.
struct BusSlot {
	std::atomic<uint32_t> sequence {0};
	std::array<std::atomic<uint64_t>, kPayloadWords> words {};
	// Cheap "is this bus occupied" flag for the audio thread. Written after each commit.
	std::atomic<bool> occupied {false};
};

// Static storage is zero-initialised, which is a valid empty node (type Empty = 0).
std::array<BusSlot, kVideoBusCount> buses;
VideoSlotAllocator<kVideoBusCount> busIds;

bool validBusId(int busId) {
	return busId >= 1 && busId <= kVideoBusCount;
}

void commitNode(int busId, const VideoBusNode& node) {
	if (!validBusId(busId)) {
		return;
	}

	uint64_t local[kPayloadWords] = {};
	std::memcpy(local, &node, sizeof(VideoBusNode));

	BusSlot& slot = buses[static_cast<size_t>(busId - 1)];
	const uint32_t sequence = slot.sequence.load(std::memory_order_relaxed);
	slot.sequence.store(sequence + 1u, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	for (size_t i = 0; i < kPayloadWords; ++i) {
		slot.words[i].store(local[i], std::memory_order_relaxed);
	}
	slot.sequence.store(sequence + 2u, std::memory_order_release);
	slot.occupied.store(node.type != VideoBusNodeType::Empty, std::memory_order_release);
}

void publishGenerator(int busId, const VideoParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::Generator;
	node.generator = params;
	commitNode(busId, node);
}
}

void videoBusPublish(int busId, const VideoParams& params) {
	publishGenerator(busId, params);
}

void videoBusPublishDeflectionYoke(int busId, int sourceBusId, const DeflectionYokeParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::DeflectionYoke;
	node.sourceBusId = sourceBusId;
	node.deflectionYoke = params;
	commitNode(busId, node);
}

void videoBusPublishLineField(int busId, int sourceBusId, const LineFieldParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::LineField;
	node.sourceBusId = sourceBusId;
	node.lineField = params;
	commitNode(busId, node);
}

void videoBusPublishExternalFrame(int busId, int externalFrameId) {
	VideoBusNode node;
	node.type = VideoBusNodeType::ExternalFrame;
	node.externalFrameId = externalFrameId;
	commitNode(busId, node);
}

void videoBusPublishScopeTrace(int busId, int scopeTraceSlotId, const ScopeTraceParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::ScopeTrace;
	node.scopeTraceSlotId = scopeTraceSlotId;
	node.scopeTrace = params;
	commitNode(busId, node);
}

void videoBusPublishColorLab(int busId, int sourceBusId, const ColorLabParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::ColorLab;
	node.sourceBusId = sourceBusId;
	node.colorLab = params;
	commitNode(busId, node);
}

void videoBusPublishKeyer(int busId, int sourceBusId, const KeyerParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::Keyer;
	node.sourceBusId = sourceBusId;
	node.keyer = params;
	commitNode(busId, node);
}

void videoBusPublishBusMixer(int busId, int sourceBusId, const BusMixerParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::BusMixer;
	node.sourceBusId = sourceBusId;
	node.busMixer = params;
	commitNode(busId, node);
}

void videoBusPublishWaveShaper(int busId, int sourceBusId, const WaveShaperParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::WaveShaper;
	node.sourceBusId = sourceBusId;
	node.waveShaper = params;
	commitNode(busId, node);
}

void videoBusPublishFreezeFrame(int busId, int sourceBusId, const FreezeFrameParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::FreezeFrame;
	node.sourceBusId = sourceBusId;
	node.freezeFrame = params;
	commitNode(busId, node);
}

void videoBusPublishScanProcessor(int busId, int sourceBusId, const ScanProcessorParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::ScanProcessor;
	node.sourceBusId = sourceBusId;
	node.scanProcessor = params;
	commitNode(busId, node);
}

void videoBusClear(int busId) {
	commitNode(busId, VideoBusNode {});
}

bool videoBusReadNode(int busId, VideoBusNode& node, bool* contended) {
	if (contended) {
		*contended = false;
	}
	if (!validBusId(busId)) {
		return false;
	}

	const BusSlot& slot = buses[static_cast<size_t>(busId - 1)];
	uint64_t local[kPayloadWords];
	for (int attempt = 0; attempt < kMaxReadAttempts; ++attempt) {
		const uint32_t before = slot.sequence.load(std::memory_order_acquire);
		if (before & 1u) {
			continue;
		}
		for (size_t i = 0; i < kPayloadWords; ++i) {
			local[i] = slot.words[i].load(std::memory_order_relaxed);
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		if (slot.sequence.load(std::memory_order_relaxed) == before) {
			std::memcpy(&node, local, sizeof(VideoBusNode));
			return node.type != VideoBusNodeType::Empty;
		}
	}
	// The writer kept interrupting (for instance the audio thread was descheduled mid-write). The bus is
	// not known to be empty: say so, so the Display can keep its last picture instead of flashing NO VIDEO.
	if (contended) {
		*contended = true;
	}
	return false;
}

bool videoBusRead(int busId, VideoParams& params) {
	VideoBusNode node;
	if (!videoBusReadNode(busId, node) || node.type != VideoBusNodeType::Generator) {
		return false;
	}
	params = node.generator;
	return true;
}

bool videoBusIsActive(int busId) {
	if (!validBusId(busId)) {
		return false;
	}
	return buses[static_cast<size_t>(busId - 1)].occupied.load(std::memory_order_acquire);
}

int videoBusAllocate(int preferredId) {
	return busIds.claim(preferredId);
}

void videoBusRelease(int busId) {
	if (!validBusId(busId)) {
		return;
	}
	videoBusClear(busId);
	busIds.release(busId);
}

int videoBusIdFromVoltage(float voltage) {
	const float rounded = std::round(voltage);
	if (!(rounded >= 1.f && rounded <= static_cast<float>(kVideoBusCount))) {
		return 0;
	}
	return static_cast<int>(rounded);
}

VideoBusOwner::VideoBusOwner() : busId(videoBusAllocate(0)) {}

VideoBusOwner::~VideoBusOwner() {
	videoBusRelease(busId);
}

void VideoBusOwner::claimPreferred(int preferredId) {
	if (preferredId == busId || !validBusId(preferredId)) {
		return;
	}
	videoBusRelease(busId);
	busId = videoBusAllocate(preferredId);
}

void videoBusPublishColorizer(int busId, int sourceBusId, const ColorizerParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::Colorizer;
	node.sourceBusId = sourceBusId;
	node.colorizer = params;
	commitNode(busId, node);
}

void videoBusPublishRamps(int busId, const RampsParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::Ramps;
	node.sourceBusId = 0;
	node.ramps = params;
	commitNode(busId, node);
}

void videoBusPublishFeedback(int busId, int sourceBusId, const FeedbackParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::Feedback;
	node.sourceBusId = sourceBusId;
	node.feedback = params;
	commitNode(busId, node);
}

void videoBusPublishFocus(int busId, int sourceBusId, const FocusParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::Focus;
	node.sourceBusId = sourceBusId;
	node.focus = params;
	commitNode(busId, node);
}

void videoBusPublishRotator(int busId, int sourceBusId, const RotatorParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::Rotator;
	node.sourceBusId = sourceBusId;
	node.rotator = params;
	commitNode(busId, node);
}

void videoBusPublishShapes(int busId, const ShapesParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::Shapes;
	node.sourceBusId = 0;
	node.shapes = params;
	commitNode(busId, node);
}

void videoBusPublishVideoMixer(int busId, const VideoMixerParams& params) {
	VideoBusNode node;
	node.type = VideoBusNodeType::VideoMixer;
	node.sourceBusId = 0;
	node.videoMixer = params;
	commitNode(busId, node);
}
