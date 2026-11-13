#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

// Scan-time signal capture. See docs/architecture.md, "Scan-time signal model".
//
// A module's process() pushes one float per channel per sample into its ring (single writer,
// audio thread, lock-free). The renderer (UI thread) reads a copy of a window of the ring:
//
// - readFrame(): the most recent complete raster frame. The raster is nominal: 60 frames/s,
//   each frame is one render height of lines and the frame period T = 1/60 s. Samples per
//   frame S = round(sampleRate / 60). The window starts on a frame boundary of the sample
//   clock, so a signal's phase against the raster is stable.
// - readLatest(): the newest N samples, for traces that use a time window in seconds.
//
// Samples are read back with linear interpolation. Values are stored as relaxed atomics, so
// a reader never has a data race. A reader that has been overrun by the writer (more than the
// ring capacity written during its copy) gets false and tries again on its next call.

// Channels 0..3. Each module gives them its own meaning: DeflectionYokeSignalChannel and ScanProcessorSignalChannel
// in video_engine.hpp; Scope Trace pushes X, Y and Z in channels 0..2.
constexpr int kSignalRingChannels = 4;
constexpr int kSignalRingCapacity = 1 << 15;  // samples per channel (power of two)
constexpr int kSignalRingSlotCount = 64;
constexpr double kRasterFrameRate = 60.0;     // nominal frames per second
constexpr double kRasterFramePeriod = 1.0 / kRasterFrameRate;
// Most samples one readLatest()/readTriggered() call returns. A window never needs more than half the ring.
constexpr int kSignalRingMaxCopy = kSignalRingCapacity / 2;
// Trigger stamps kept per ring. Readers use the newest stamp that has a full window after it.
constexpr int kSignalRingTriggerHistory = 16;

// Copy of one window taken by a reader. Channel-major: data[channel * length + index].
struct SignalWindow {
	int channels = 0;
	int length = 0;                  // samples per channel
	int framePeriodSamples = 0;      // S for readFrame() windows, 0 for readLatest()
	double framePeriodSeconds = kRasterFramePeriod;  // period the frame was read with
	float sampleRate = 0.f;
	std::vector<float> data;

	bool valid() const {
		return length > 0 && channels > 0;
	}

	// Value at sample position pos (samples from the window start), linearly interpolated.
	float sampleAt(int channel, double pos) const;

	// Value at scan time t (seconds from the start of the frame) for a readFrame() window.
	// t is mapped as t / T * S, where T is framePeriodSeconds, so one frame spans exactly S samples.
	float valueAtScanTime(int channel, float t) const;
};

class SignalRing {
public:
	// Writer side (audio thread). Pushes one sample; values holds kSignalRingChannels floats.
	void push(const float* values);
	// Writer side. Sets the rate used to size frames. Cheap enough to call every sample.
	void setSampleRate(float sampleRate);
	// Writer side. Ignores everything written so far (a trace reset). The ring keeps writing.
	void clear();
	// Construction/destruction thread, with no writer running. Back to the state of a ring nobody has used:
	// no samples, no rate, no trigger stamps and a write count of 0 (clear() keeps the write count).
	void reset();

	// Reader side (any thread). False until a complete frame has been written. The frame is
	// periodSeconds long (the nominal 60 Hz raster by default, or a field period such as
	// crt::fieldSeconds()).
	bool readFrame(SignalWindow& out, double periodSeconds = kRasterFramePeriod) const;
	// Reader side. Newest `count` samples (fewer is not returned: false if not enough).
	bool readLatest(int count, SignalWindow& out) const;
	// Writer side. Stamps the newest written sample as a trigger point (call after push()).
	void markTrigger();
	// Reader side. `count` samples starting at the newest trigger stamp that has `count` samples
	// written after it (and is still in the ring). False if there is none.
	bool readTriggered(int count, SignalWindow& out) const;

	uint64_t writeCount() const {
		return writes.load(std::memory_order_acquire);
	}
	float sampleRate() const {
		return rate.load(std::memory_order_relaxed);
	}

private:
	std::atomic<float> samples[kSignalRingChannels * kSignalRingCapacity] {};
	std::atomic<uint64_t> writes {0};
	std::atomic<uint64_t> origin {0};
	std::atomic<float> rate {0.f};
	std::atomic<uint64_t> stamps[kSignalRingTriggerHistory] {};
	std::atomic<uint64_t> stampCount {0};

	void copyRange(uint64_t start, int count, SignalWindow& out) const;
};

// Per-module rings, claimed in the constructor and released in the destructor (UI/construction
// thread). Push, setSampleRate and clear run on the audio thread; reads run on the UI thread. Release
// resets the ring (SignalRing::reset), so the next module to claim the slot starts from a ring like a new
// one, not from the previous owner's write count, rate or trigger stamps.
// Returns 0 from allocate when all kSignalRingSlotCount slots are in use. Slot 0 is not a ring: every
// function below ignores it, so a module that got 0 runs but records nothing.
int signalRingAllocate();
// signalRingAllocate() for the audio thread: never waits. Returns 0 when no slot is free or the pool is
// busy at that instant, so a module whose constructor got 0 can call it again from process().
int signalRingTryAllocate();
void signalRingRelease(int slotId);
void signalRingPush(int slotId, const float* values);
void signalRingSetSampleRate(int slotId, float sampleRate);
void signalRingClear(int slotId);
bool signalRingReadFrame(int slotId, SignalWindow& out, double periodSeconds = kRasterFramePeriod);
bool signalRingReadLatest(int slotId, int count, SignalWindow& out);
// Trigger stamps: the writer (audio thread) marks the sample it just pushed; the reader takes a
// window that starts there. See SignalRing::markTrigger and readTriggered.
void signalRingMarkTrigger(int slotId);
bool signalRingReadTriggered(int slotId, int count, SignalWindow& out);
// Sample rate the writer announced for this slot (0 until set).
float signalRingRate(int slotId);
// Newest `seconds` of audio, converted with the slot's sample rate. False until the rate is known.
bool signalRingReadSeconds(int slotId, double seconds, SignalWindow& out);
