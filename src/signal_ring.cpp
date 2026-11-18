#include "signal_ring.hpp"
#include "video_slot_allocator.hpp"

#include <algorithm>
#include <cmath>

#include "video_clock.hpp"

namespace {
constexpr uint64_t kCapacity = static_cast<uint64_t>(kSignalRingCapacity);
constexpr int kMaxCopy = kSignalRingMaxCopy;  // a window never needs more than half the ring
}

float SignalWindow::sampleAt(int channel, double pos) const {
	if (!valid() || channel < 0 || channel >= channels) {
		return 0.f;
	}
	const double last = static_cast<double>(length - 1);
	pos = std::clamp(pos, 0.0, last);
	const int i0 = std::clamp(static_cast<int>(std::floor(pos)), 0, length - 1);
	const int i1 = std::min(i0 + 1, length - 1);
	const float frac = static_cast<float>(pos - static_cast<double>(i0));
	const float* base = data.data() + static_cast<size_t>(channel) * static_cast<size_t>(length);
	return base[i0] + (base[i1] - base[i0]) * frac;
}

float SignalWindow::valueAtScanTime(int channel, float t) const {
	if (fieldWindow) {
		return sampleAt(channel, startFraction + static_cast<double>(t) * static_cast<double>(sampleRate));
	}
	// readFrame() windows hold S + 2 samples: the frame plus one sample of interpolation margin.
	if (framePeriodSamples <= 0) {
		return 0.f;
	}
	const double pos = static_cast<double>(t) / framePeriodSeconds * static_cast<double>(framePeriodSamples);
	return sampleAt(channel, pos);
}

void SignalRing::push(const float* values) {
	const uint64_t n = writes.load(std::memory_order_relaxed);
	const size_t index = static_cast<size_t>(n & (kCapacity - 1));
	for (int ch = 0; ch < kSignalRingChannels; ++ch) {
		samples[static_cast<size_t>(ch) * kSignalRingCapacity + index].store(values[ch], std::memory_order_relaxed);
	}
	writes.store(n + 1, std::memory_order_release);
}

void SignalRing::pushAt(const float* values, int64_t frame) {
	const uint64_t n = writes.load(std::memory_order_relaxed);
	const int64_t expected = static_cast<int64_t>(n) + frameOffset.load(std::memory_order_relaxed);
	if (frame != expected) {
		// The frame counter did not advance by one: restart the stamping here and hide what came before.
		frameOffset.store(frame - static_cast<int64_t>(n), std::memory_order_relaxed);
		origin.store(n, std::memory_order_release);
	}
	push(values);
}

void SignalRing::setSampleRate(float sampleRate) {
	if (rate.load(std::memory_order_relaxed) != sampleRate) {
		rate.store(sampleRate, std::memory_order_relaxed);
	}
}

void SignalRing::clear() {
	origin.store(writes.load(std::memory_order_relaxed), std::memory_order_release);
}

void SignalRing::reset() {
	// Write count first, so a reader that catches this between the two stores sees an empty ring
	// (written < first) rather than the old samples. The stamps need no clearing: readTriggered() only
	// looks at the newest min(stampCount, history) of them, and every one of those is written again first.
	writes.store(0, std::memory_order_release);
	origin.store(0, std::memory_order_release);
	rate.store(0.f, std::memory_order_relaxed);
	frameOffset.store(0, std::memory_order_relaxed);
	stampCount.store(0, std::memory_order_release);
}

void SignalRing::copyRange(uint64_t start, int count, SignalWindow& out) const {
	out.channels = kSignalRingChannels;
	out.length = count;
	out.fieldWindow = false;
	out.startFraction = 0.0;
	out.field = 0;
	out.data.resize(static_cast<size_t>(kSignalRingChannels) * static_cast<size_t>(count));
	for (int ch = 0; ch < kSignalRingChannels; ++ch) {
		const size_t channelBase = static_cast<size_t>(ch) * kSignalRingCapacity;
		float* dst = out.data.data() + static_cast<size_t>(ch) * static_cast<size_t>(count);
		for (int i = 0; i < count; ++i) {
			const size_t index = static_cast<size_t>((start + static_cast<uint64_t>(i)) & (kCapacity - 1));
			dst[i] = samples[channelBase + index].load(std::memory_order_relaxed);
		}
	}
}

bool SignalRing::readFrame(SignalWindow& out, double periodSeconds) const {
	const float sr = rate.load(std::memory_order_relaxed);
	if (!(sr > 0.f) || !(periodSeconds > 0.0)) {
		return false;
	}
	const int frameSamples = static_cast<int>(std::lround(static_cast<double>(sr) * periodSeconds));
	if (frameSamples < 2 || frameSamples + 2 > kMaxCopy) {
		return false;
	}

	const uint64_t written = writes.load(std::memory_order_acquire);
	const uint64_t first = origin.load(std::memory_order_acquire);
	// Newest frame boundary f with f * S <= written - 2, so the window (one frame plus margin)
	// is complete. The window is the frame that ends at boundary f.
	const uint64_t S = static_cast<uint64_t>(frameSamples);
	if (written < first + S + 2) {
		return false;
	}
	const uint64_t boundary = (written - 2) / S;
	if (boundary < 1) {
		return false;
	}
	const uint64_t start = (boundary - 1) * S;
	if (start < first) {
		return false;
	}

	const int length = frameSamples + 2;
	copyRange(start, length, out);
	out.framePeriodSamples = frameSamples;
	out.framePeriodSeconds = periodSeconds;
	out.sampleRate = sr;

	// Copy is valid only if the writer did not wrap onto the first sample we read.
	if (writes.load(std::memory_order_acquire) > start + kCapacity) {
		return false;
	}
	return true;
}

bool SignalRing::readField(SignalWindow& out, int lines) const {
	const float sr = rate.load(std::memory_order_relaxed);
	if (!(sr > 0.f) || lines < 1) {
		return false;
	}
	const vclock::Clock clock(static_cast<double>(sr), lines);
	const double spf = clock.samplesPerField();
	// Interpolating anywhere in the field reads at most floor(startFraction + spf) + 1, so this many samples.
	const int length = static_cast<int>(std::floor(spf)) + 3;
	if (length < 3 || length > kMaxCopy) {
		return false;
	}

	const uint64_t written = writes.load(std::memory_order_acquire);
	const uint64_t first = origin.load(std::memory_order_acquire);
	const int64_t offset = frameOffset.load(std::memory_order_relaxed);
	// The window of field n begins at ring index floor(n x spf) - offset and holds `length` samples; it is
	// complete when that index + length <= written. Take the newest such field.
	const double newestStart = static_cast<double>(static_cast<int64_t>(written) + offset - length);
	if (newestStart < 0.0) {
		return false;
	}
	int64_t n = static_cast<int64_t>(std::floor((newestStart + 1.0) / spf));
	while (n >= 0 && static_cast<int64_t>(std::floor(static_cast<double>(n) * spf + vclock::kSnap)) - offset + length > static_cast<int64_t>(written)) {
		--n;
	}
	if (n < 0) {
		return false;
	}
	const double startFrame = static_cast<double>(n) * spf;
	const int64_t baseFrame = static_cast<int64_t>(std::floor(startFrame + vclock::kSnap));
	const int64_t baseIndex = baseFrame - offset;
	if (baseIndex < 0 || static_cast<uint64_t>(baseIndex) < first) {
		return false;
	}

	copyRange(static_cast<uint64_t>(baseIndex), length, out);
	out.framePeriodSamples = 0;
	out.framePeriodSeconds = clock.fieldSeconds();
	out.sampleRate = sr;
	out.fieldWindow = true;
	out.startFraction = std::max(0.0, startFrame - static_cast<double>(baseFrame));
	out.field = n;
	if (writes.load(std::memory_order_acquire) > static_cast<uint64_t>(baseIndex) + kCapacity) {
		return false;
	}
	return true;
}

bool SignalRing::readSince(int64_t afterFrame, int maxCount, SignalWindow& out) const {
	const float sr = rate.load(std::memory_order_relaxed);
	if (!(sr > 0.f) || maxCount < 1) {
		return false;
	}
	const uint64_t written = writes.load(std::memory_order_acquire);
	const uint64_t first = origin.load(std::memory_order_acquire);
	const int64_t offset = frameOffset.load(std::memory_order_relaxed);
	const int64_t newest = static_cast<int64_t>(written) - 1 + offset;
	// A reader that is ahead of the writer (the ring restarted) starts again from what is there.
	if (afterFrame > newest) afterFrame = INT64_MIN / 2;
	afterFrame = std::max<int64_t>(afterFrame, INT64_MIN / 2);
	const int64_t want = afterFrame + 1 - offset;   // ring index of the first wanted sample
	int64_t start = std::max<int64_t>(want, static_cast<int64_t>(first));
	start = std::max<int64_t>(start, static_cast<int64_t>(written) - std::min(maxCount, kMaxCopy));
	if (start < 0 || start >= static_cast<int64_t>(written)) {
		return false;
	}
	const int count = static_cast<int>(static_cast<int64_t>(written) - start);
	copyRange(static_cast<uint64_t>(start), count, out);
	out.framePeriodSamples = 0;
	out.sampleRate = sr;
	out.firstFrame = start + offset;
	if (writes.load(std::memory_order_acquire) > static_cast<uint64_t>(start) + kCapacity) {
		return false;
	}
	return true;
}

bool SignalRing::readLatest(int count, SignalWindow& out) const {
	const uint64_t written = writes.load(std::memory_order_acquire);
	const uint64_t first = origin.load(std::memory_order_acquire);
	const uint64_t available = written > first ? written - first : 0u;
	const int wanted = std::min(count, kMaxCopy);
	const int n = static_cast<int>(std::min<uint64_t>(available, static_cast<uint64_t>(wanted)));
	if (n < 2) {
		return false;
	}

	const uint64_t start = written - static_cast<uint64_t>(n);
	copyRange(start, n, out);
	out.framePeriodSamples = 0;
	out.sampleRate = rate.load(std::memory_order_relaxed);

	if (writes.load(std::memory_order_acquire) > start + kCapacity) {
		return false;
	}
	return true;
}

void SignalRing::markTrigger() {
	const uint64_t written = writes.load(std::memory_order_relaxed);
	if (written == 0) {
		return;
	}
	const uint64_t n = stampCount.load(std::memory_order_relaxed);
	// The stamp is the index of the newest sample, the first sample of a window that starts here.
	stamps[n % kSignalRingTriggerHistory].store(written - 1, std::memory_order_relaxed);
	stampCount.store(n + 1, std::memory_order_release);
}

bool SignalRing::readTriggered(int count, SignalWindow& out) const {
	const int wanted = std::min(count, kMaxCopy);
	if (wanted < 2) {
		return false;
	}
	const uint64_t n = stampCount.load(std::memory_order_acquire);
	const uint64_t written = writes.load(std::memory_order_acquire);
	const uint64_t first = origin.load(std::memory_order_acquire);
	const uint64_t recent = std::min<uint64_t>(n, kSignalRingTriggerHistory);
	for (uint64_t k = 0; k < recent; ++k) {
		const uint64_t stamp = stamps[(n - 1 - k) % kSignalRingTriggerHistory].load(std::memory_order_relaxed);
		if (stamp < first || written < stamp + static_cast<uint64_t>(wanted)) {
			continue;  // too new: its window is not complete yet
		}
		if (written - stamp > kCapacity - static_cast<uint64_t>(wanted)) {
			break;  // older stamps are overwritten too
		}
		copyRange(stamp, wanted, out);
		out.framePeriodSamples = 0;
		out.sampleRate = rate.load(std::memory_order_relaxed);
		if (writes.load(std::memory_order_acquire) > stamp + kCapacity) {
			return false;
		}
		return true;
	}
	return false;
}

namespace {
SignalRing rings[kSignalRingSlotCount];
VideoSlotAllocator<kSignalRingSlotCount> slotIds;

bool validSlot(int slotId) {
	return slotId >= 1 && slotId <= kSignalRingSlotCount;
}
}

int signalRingAllocate() {
	return slotIds.claim(0);
}

int signalRingTryAllocate() {
	return slotIds.tryClaim();
}

void signalRingRelease(int slotId) {
	if (!validSlot(slotId)) {
		return;
	}
	// The next module to claim this slot gets a ring like a new one: no leftover rate or trigger stamps,
	// and a write count of 0, so readFrame()'s frame boundaries (multiples of S counted from the first
	// sample) sit where they do on a ring nobody has used.
	rings[slotId - 1].reset();
	slotIds.release(slotId);
}

void signalRingPush(int slotId, const float* values) {
	if (validSlot(slotId)) {
		rings[slotId - 1].push(values);
	}
}

void signalRingSetSampleRate(int slotId, float sampleRate) {
	if (validSlot(slotId)) {
		rings[slotId - 1].setSampleRate(sampleRate);
	}
}

void signalRingClear(int slotId) {
	if (validSlot(slotId)) {
		rings[slotId - 1].clear();
	}
}

bool signalRingReadFrame(int slotId, SignalWindow& out, double periodSeconds) {
	return validSlot(slotId) && rings[slotId - 1].readFrame(out, periodSeconds);
}

void signalRingPushAt(int slotId, const float* values, int64_t frame) {
	if (validSlot(slotId)) {
		rings[slotId - 1].pushAt(values, frame);
	}
}

bool signalRingReadField(int slotId, SignalWindow& out, int lines) {
	return validSlot(slotId) && rings[slotId - 1].readField(out, lines);
}

bool signalRingReadSince(int slotId, int64_t afterFrame, int maxCount, SignalWindow& out) {
	return validSlot(slotId) && rings[slotId - 1].readSince(afterFrame, maxCount, out);
}

bool signalRingReadLatest(int slotId, int count, SignalWindow& out) {
	return validSlot(slotId) && rings[slotId - 1].readLatest(count, out);
}

void signalRingMarkTrigger(int slotId) {
	if (validSlot(slotId)) {
		rings[slotId - 1].markTrigger();
	}
}

bool signalRingReadTriggered(int slotId, int count, SignalWindow& out) {
	return validSlot(slotId) && rings[slotId - 1].readTriggered(count, out);
}

float signalRingRate(int slotId) {
	return validSlot(slotId) ? rings[slotId - 1].sampleRate() : 0.f;
}

bool signalRingReadSeconds(int slotId, double seconds, SignalWindow& out) {
	if (!validSlot(slotId)) {
		return false;
	}
	const SignalRing& ring = rings[slotId - 1];
	const double rate = static_cast<double>(ring.sampleRate());
	if (!(rate > 0.0) || !(seconds > 0.0)) {
		return false;
	}
	return ring.readLatest(static_cast<int>(std::lround(seconds * rate)), out);
}
