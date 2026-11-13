#pragma once

#include <algorithm>
#include <cmath>

// Video Mixer (VRU-22) control maths, kept apart from the module so tests can drive it without Rack.
// The picture maths lives in the renderer (VideoGraphRenderer::renderVideoMixerFrame); this file decides
// which input is live, which is next, and how far the next one has come up.

constexpr int kVideoMixerInputs = 4;

// TIME knob (0..1) to the seconds a whole take lasts, 0.02 s to 20 s, exponential.
inline float videoMixerTakeSeconds(float knob) {
	return 0.02f * std::pow(1000.f, std::clamp(knob, 0.f, 1.f));
}

// The input the NEXT CV picks: 0 to 10 V in four equal bands, A to D. Below 0 V is A, above 10 V is D.
inline int videoMixerInputFromVolts(float volts) {
	if (!(volts > 0.f)) return 0;
	return std::clamp(static_cast<int>(volts / 2.5f), 0, kVideoMixerInputs - 1);
}

// SHIFT turns every input's hue together: 10 V is once round the colour wheel, as on the Rotator's ANGLE. The
// renderer wraps the hue, so the sum may leave 0..1.
inline float videoMixerShiftedHue(float hue, float shiftVolts) {
	return hue + shiftVolts * 0.1f;
}

// The patched input that follows `from` round the four, skipping `skip` and every unpatched one (bit i of
// patched is input i). -1 when there is none.
inline int videoMixerFollowing(int from, int skip, unsigned patched) {
	for (int step = 1; step <= kVideoMixerInputs; ++step) {
		const int index = (from + step) % kVideoMixerInputs;
		if (index != skip && (patched & (1u << index)) != 0u) return index;
	}
	return -1;
}

// The lever (FADER knob plus CV, 0..1) is a studio T-bar. Its position is how far the next picture has come up,
// counted from the end it last left, so `flipped` says which end is home. At the far end the two pictures swap and
// home moves to the end the lever is at, which is what makes a triangle LFO on FADER go back and forth.
struct VideoMixerState {
	static constexpr double kEnd = 0.005;   // within this of the far end counts as arrived
	static constexpr double kHome = 0.02;   // within this of home counts as back

	int onAir = 0;          // the live input, 0 to 3 for A to D (the code keeps this older name)
	int next = 1;
	bool flipped = false;   // lever home is the top end
	bool armed = true;      // the lever drives the picture; false after a cut or take until it is back home
	bool taking = false;    // AUTO is running
	double take = 0.0;      // how far AUTO has got, 0..1
	float mix = 0.f;        // result of the last step: how far the next picture has come up

	struct Inputs {
		float lever = 0.f;       // FADER knob plus CV, 0..1
		bool cut = false;        // a CUT trigger or press this tick
		bool take = false;       // an AUTO trigger or press this tick
		bool step = false;       // a NEXT trigger or press this tick
		int select = -1;         // input the NEXT CV picks, -1 when that jack is empty
		bool advance = false;    // ADVANCE
		float takeSeconds = 1.f; // the whole of a take
		unsigned patched = 0xFu; // bit i set when input i has a picture
	};

	void reset() {
		*this = VideoMixerState();
	}

	// One tick of dt seconds.
	void step(const Inputs& in, double dt) {
		// NEXT: the voltage picks the input outright; otherwise a trigger steps to the following patched one.
		if (in.select >= 0) {
			next = std::clamp(in.select, 0, kVideoMixerInputs - 1);
		}
		else if (in.step) {
			const int following = videoMixerFollowing(next, onAir, in.patched);
			if (following >= 0) next = following;
		}
		else if (!taking && mix <= 0.f) {
			// At rest, next is a patched input other than the live one whenever there is one, so pulling a cable
			// out (or a fresh module whose B is empty) never leaves the lever fading toward nothing. Never mid-take.
			const bool usable = next != onAir && (in.patched & (1u << next)) != 0u;
			if (!usable) {
				const int following = videoMixerFollowing(next, onAir, in.patched);
				if (following >= 0) next = following;
			}
		}

		const double lever = std::clamp(static_cast<double>(in.lever), 0.0, 1.0);
		const double fromHome = flipped ? 1.0 - lever : lever;

		if (in.cut) {
			// A cut makes the next picture live at once, and ends a take that is running.
			finish(in);
			taking = false;
			rearm(fromHome);
			mix = 0.f;
			return;
		}
		if (in.take && !taking) {
			taking = true;
			take = mix;
		}
		if (taking) {
			take += dt / std::max(static_cast<double>(in.takeSeconds), 1e-4);
			if (take >= 1.0) {
				finish(in);
				taking = false;
				rearm(fromHome);
				mix = 0.f;
			}
			else {
				mix = static_cast<float>(take);
			}
			return;
		}
		if (!armed) {
			mix = 0.f;
			if (fromHome <= kHome) armed = true;
			return;
		}
		if (fromHome >= 1.0 - kEnd) {
			finish(in);
			flipped = !flipped;
			mix = 0.f;
			return;
		}
		mix = static_cast<float>(fromHome);
	}

private:
	// The next picture goes live. With ADVANCE the picture after it is up next, so a clock walks round the
	// patched inputs; without, the picture that was live is next, so two pictures trade places.
	void finish(const Inputs& in) {
		const int old = onAir;
		onAir = next;
		const int following = in.advance ? videoMixerFollowing(onAir, onAir, in.patched) : -1;
		next = following >= 0 ? following : old;
	}

	// After a cut or a take the lever is read from where it stands. If it is not at home the new picture stays
	// whole until the lever comes back, instead of jumping to a part-way mix.
	void rearm(double fromHome) {
		armed = fromHome <= kHome;
	}
};
