#pragma once

// A beam graph shared by the API tests: an oscillator with feedback, a Scope Trace, a Deflection Yoke on the
// oscillator and a Bus Mixer of both, on buses base to base+3 (the mixer is base+3).

#include "signal_ring.hpp"
#include "video_bus.hpp"

#include <cmath>

struct BeamRig {
	int base = 0;
	int scopeSlot = 0;
	int yokeSlot = 0;
	int tick = 0;
	// bus base oscillator with feedback, base+1 Scope Trace, base+2 Deflection Yoke on base, base+3 Bus Mixer of both.
	void build(int b) {
		base = b;
		scopeSlot = signalRingAllocate();
		yokeSlot = signalRingAllocate();
		signalRingSetSampleRate(scopeSlot, 48000.f);
		signalRingSetSampleRate(yokeSlot, 48000.f);
		tick = 0;
		push(4800);
		VideoParams v;
		v.phase = 0.15f;
		v.pattern = 1;
		v.feedback = 0.5f;
		videoBusPublish(base, v);
		ScopeTraceParams st;
		st.decay = 0.8f;
		videoBusPublishScopeTrace(base + 1, scopeSlot, st);
		DeflectionYokeParams dy;
		dy.signalSlotId = yokeSlot;
		dy.persist = 0.7f;
		dy.lines = 120;
		dy.depth = 0.5f;
		videoBusPublishDeflectionYoke(base + 2, base, dy);
		BusMixerParams m;
		m.busB = base + 1;
		m.mode = 1;
		m.mix = 0.5f;
		videoBusPublishBusMixer(base + 3, base + 2, m);
	}
	void push(int count) {
		for (int n = 0; n < count; ++n) {
			const double t = static_cast<double>(tick * 100000 + n);
			const float sc[kSignalRingChannels] = {static_cast<float>(0.8 * std::sin(0.031 * t)), static_cast<float>(0.8 * std::cos(0.047 * t)), 1.f, 0.f};
			signalRingPush(scopeSlot, sc);
			const float yk[kSignalRingChannels] = {static_cast<float>(0.2 * std::sin(0.011 * t)), static_cast<float>(0.2 * std::sin(0.013 * t)), 0.f, 0.f};
			signalRingPush(yokeSlot, yk);
		}
	}
	void nextTick() {
		++tick;
		push(800);
	}
	void release() {
		for (int k = 0; k < 4; ++k) {
			videoBusClear(base + k);
		}
		signalRingRelease(scopeSlot);
		signalRingRelease(yokeSlot);
	}
};
