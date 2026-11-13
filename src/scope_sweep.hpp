#pragma once

// Scope Trace sweep generator: the ramp a real scope's timebase would produce, for the SWEEP and
// SYNC outputs. Pure maths, no Rack types, one call per audio sample.
//
// A sweep runs for one scope window (see scopeWindowSeconds). While it runs, further triggers are
// ignored (holdoff). When it ends the ramp snaps back to -5 V and waits for the next trigger; if
// none arrives within one window it starts by itself, like a scope's AUTO mode, so there is
// always a sweep to patch. The ramp runs in every display mode, XY and Polar included.
//
// Counting is done in whole samples. The window is a whole number of samples N when the caller's
// dt is exact, but dt is a float, so every comparison allows half a sample of slack. Without it,
// a sweep of 240 samples could run 241 depending on rounding.
struct ScopeSweep {
	double phase = 0.0;    // 0..1 along the window while running
	bool running = false;  // a sweep is in progress (holdoff: triggers are ignored)
	double idle = 0.0;     // seconds since the last sweep ended
	bool restartPending = false;  // restart() was called; the next process() starts a sweep

	// Advance by dt seconds. windowSeconds is the current scope window. triggerEdge is true on the
	// sample a trigger fired. Returns true on the sample a new sweep starts (that sample's ramp is
	// -5 V; the ramp then rises by 10 V / N per sample and the sample after the last step is the
	// end: ramp back at -5 V, running false). A trigger on the end sample starts the next sweep
	// on that same sample. A window that is not positive (or not a number) stops the sweep.
	bool process(double dt, double windowSeconds, bool triggerEdge) {
		if (!(windowSeconds > 0.0) || !(dt > 0.0)) {
			stop();
			return false;
		}
		if (restartPending) {
			restartPending = false;
			begin();
			return true;
		}
		if (running) {
			phase += dt / windowSeconds;
			if (phase < 1.0 - 0.5 * dt / windowSeconds) {
				return false;
			}
			// The sweep is over. Idle starts counting from the next sample.
			running = false;
			phase = 0.0;
			idle = 0.0;
			if (triggerEdge) {
				begin();
				return true;
			}
			return false;
		}
		idle += dt;
		if (triggerEdge || idle >= windowSeconds - 0.5 * dt) {
			begin();
			return true;
		}
		return false;
	}

	// -5 V at the start of the window, rising to just under +5 V on the last sample of the sweep;
	// -5 V while waiting.
	float rampVolts() const {
		if (!running) {
			return -5.f;
		}
		const double p = phase < 0.0 ? 0.0 : (phase > 1.0 ? 1.0 : phase);
		return static_cast<float>(-5.0 + 10.0 * p);
	}

	// RESET: start a new sweep now. The next process() call reports the start, even in the middle
	// of a sweep (it overrides the holdoff).
	void restart() {
		restartPending = true;
	}

private:
	void begin() {
		running = true;
		phase = 0.0;
		idle = 0.0;
	}

	void stop() {
		running = false;
		phase = 0.0;
		idle = 0.0;
		restartPending = false;
	}
};
