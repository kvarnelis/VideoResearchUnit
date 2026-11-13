// Headless Scope Trace module check against Rack's real Module/Port/JSON code. No GUI or running Rack.
// CV on DWELL and on the other new jacks changes the value process() publishes; an old patch's JSON, which
// has none of the new ids, loads with the same values for the old controls and no CV effect.
#include "../src/ScopeTrace.cpp"
#include <cstdio>

Plugin* pluginInstance = nullptr;

int main() {
	int fails = 0, checks = 0;
	auto check = [&](bool v, const char* msg) { ++checks; if (!v) { ++fails; std::fprintf(stderr, "FAIL: %s\n", msg); } };
	auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
	rack::engine::Module::ProcessArgs args{};
	args.sampleRate = 48000.f;
	args.sampleTime = 1.f / 48000.f;

	// Module::toJson and fromJson reach params through APP->engine, so give the context a real Engine.
	rack::Context context;
	context.engine = new rack::engine::Engine;
	rack::contextSet(&context);

	// toJson and fromJson read the module's model and plugin, which Rack sets when it creates a module.
	Plugin plugin;
	plugin.slug = "VideoResearchUnit";
	modelScopeTrace->plugin = &plugin;
	modelScopeTrace->slug = "ScopeTrace";

	ScopeTrace m;
	check(m.params.size() == 28 && m.inputs.size() == 18, "28 params and 18 inputs");

	// No CV: the published values are the knobs.
	m.process(args);
	check(near(m.lastScope.dwell, 0.5f) && near(m.lastScope.decay, 0.65f) && near(m.lastScope.thickness, 0.4f) &&
		near(m.lastScope.intensity, 1.2f) && near(m.lastScope.bloom, 0.f), "no CV: the knobs");

	// DWELL: 0..10 V sweeps the whole knob (1/10 per volt).
	m.inputs[ScopeTrace::DWELL_INPUT].setVoltage(2.f);
	m.process(args);
	check(near(m.lastScope.dwell, 0.7f), "DWELL +2 V adds 0.2");
	m.inputs[ScopeTrace::DWELL_INPUT].setVoltage(10.f);
	m.process(args);
	check(near(m.lastScope.dwell, 1.f), "DWELL clamps at 1");
	m.inputs[ScopeTrace::DWELL_INPUT].setVoltage(-10.f);
	m.process(args);
	check(near(m.lastScope.dwell, 0.f), "DWELL clamps at 0");
	m.inputs[ScopeTrace::DWELL_INPUT].setVoltage(5.f);
	m.params[ScopeTrace::DWELL_CV_PARAM].setValue(-1.f);
	m.process(args);
	check(near(m.lastScope.dwell, 0.f), "DWELL attenuverter at -100% inverts");
	m.params[ScopeTrace::DWELL_CV_PARAM].setValue(0.5f);
	m.process(args);
	check(near(m.lastScope.dwell, 0.75f), "DWELL attenuverter at 50% halves the CV");
	m.inputs[ScopeTrace::DWELL_INPUT].setVoltage(0.f);
	m.params[ScopeTrace::DWELL_CV_PARAM].setValue(1.f);

	// INTENSITY (0..2, 1/5 per volt), BLOOM, DECAY, BEAM and the trigger level.
	m.inputs[ScopeTrace::INTENSITY_INPUT].setVoltage(2.5f);
	m.inputs[ScopeTrace::BLOOM_INPUT].setVoltage(5.f);
	m.inputs[ScopeTrace::DECAY_INPUT].setVoltage(-3.f);
	m.inputs[ScopeTrace::BEAM_INPUT].setVoltage(3.f);
	m.inputs[ScopeTrace::TRIG_LEVEL_INPUT].setVoltage(2.f);
	m.params[ScopeTrace::TRIG_LEVEL_PARAM].setValue(1.f);
	m.process(args);
	check(near(m.lastScope.intensity, 1.7f), "INTENSITY +2.5 V adds 0.5");
	check(near(m.lastScope.bloom, 1.f), "BLOOM +5 V adds 1");
	check(near(m.lastScope.decay, 0.35f), "DECAY -3 V subtracts 0.3");
	check(near(m.lastScope.thickness, 0.7f), "BEAM +3 V adds 0.3");
	check(near(m.lastScope.triggerLevel, 3.f / 5.f), "trigger level +2 V on a 1 V knob is 3 V");
	m.inputs[ScopeTrace::TRIG_LEVEL_INPUT].setVoltage(10.f);
	m.process(args);
	check(near(m.lastScope.triggerLevel, 1.f), "trigger level clamps at +5 V");
	m.inputs[ScopeTrace::INTENSITY_INPUT].setVoltage(10.f);
	m.process(args);
	check(near(m.lastScope.intensity, 2.f), "INTENSITY clamps at 2");

	// ROTATE wraps instead of clamping. Knob 1.0 with +1 V (0.2 units) is 1.2, which wraps to -0.8.
	m.inputs[ScopeTrace::INTENSITY_INPUT].setVoltage(0.f);
	m.params[ScopeTrace::ROTATION_PARAM].setValue(1.f);
	m.inputs[ScopeTrace::ROTATION_INPUT].setVoltage(1.f);
	m.process(args);
	check(near(m.lastScope.rotation, -0.8f), "ROTATE knob 1.0 with +1 V wraps to -0.8");
	m.params[ScopeTrace::ROTATION_PARAM].setValue(0.f);
	m.inputs[ScopeTrace::ROTATION_INPUT].setVoltage(5.f);
	m.process(args);
	check(near(m.lastScope.rotation, -1.f) || near(m.lastScope.rotation, 1.f), "+5 V from 0 is half a turn (+-180 degrees, the same angle)");
	// A 0..20 V sweep is two full turns: the unwrapped angle never goes backwards and never jumps.
	{
		const float pi = 3.14159265358979f;
		float previousAngle = 0.f, unwrapped = 0.f;
		bool monotonic = true, continuous = true, first = true;
		for (int step = 0; step <= 2000; ++step) {
			m.inputs[ScopeTrace::ROTATION_INPUT].setVoltage(20.f * step / 2000.f);
			m.process(args);
			const float angle = m.lastScope.rotation * pi;
			if (m.lastScope.rotation < -1.f || m.lastScope.rotation >= 1.f) {
				continuous = false;
			}
			if (!first) {
				float delta = angle - previousAngle;
				if (delta < -pi) {
					delta += 2.f * pi;  // the wrap from +180 to -180 degrees is the same angle
				}
				if (delta < 0.f) {
					monotonic = false;
				}
				if (delta > 0.05f) {
					continuous = false;  // one step is 0.01 V = 0.0063 rad; anything big is a jump
				}
				unwrapped += delta;
			}
			previousAngle = angle;
			first = false;
		}
		check(monotonic, "0 to 20 V sweep: the angle only advances");
		check(continuous, "0 to 20 V sweep: no jump, and the value stays in [-1, 1)");
		check(near(unwrapped, 4.f * pi) || std::fabs(unwrapped - 4.f * pi) < 0.01f, "0 to 20 V sweep: two full turns");
	}
	check(rasterlab_cv::wrapTurn(std::nanf("")) == 0.f && rasterlab_cv::wrapTurn(INFINITY) == 0.f, "wrapTurn: NaN and infinity give 0");
	check(near(rasterlab_cv::wrapTurn(-1.f), -1.f) && near(rasterlab_cv::wrapTurn(1.f), -1.f) && near(rasterlab_cv::wrapTurn(-3.4f), 0.6f) &&
		near(rasterlab_cv::wrapTurn(101.f), -1.f), "wrapTurn: floor-based for negatives and large values");
	m.params[ScopeTrace::ROTATION_PARAM].setValue(0.f);
	m.inputs[ScopeTrace::ROTATION_INPUT].setVoltage(0.f);

	// An old patch: only params 0..21, no CV params. Build its JSON from a fresh module, strip the new ids.
	ScopeTrace saver;
	saver.model = modelScopeTrace;
	saver.params[ScopeTrace::DWELL_PARAM].setValue(0.9f);
	saver.params[ScopeTrace::INTENSITY_PARAM].setValue(1.5f);
	saver.params[ScopeTrace::DECAY_PARAM].setValue(0.2f);
	saver.params[ScopeTrace::TRIG_LEVEL_PARAM].setValue(-2.f);
	json_t* paramsJ = saver.paramsToJson();
	json_t* oldParams = json_array();
	size_t i;
	json_t* entry;
	json_array_foreach(paramsJ, i, entry) {
		if (json_integer_value(json_object_get(entry, "id")) < 22) {
			json_array_append(oldParams, entry);
		}
	}
	json_t* patch = oldParams;  // Module::paramsFromJson takes the params array itself
	check(json_array_size(oldParams) == 22, "the old patch has 22 params");

	ScopeTrace loaded;
	loaded.model = modelScopeTrace;
	loaded.paramsFromJson(patch);
	json_decref(patch);
	check(near(loaded.params[ScopeTrace::DWELL_PARAM].getValue(), 0.9f) && near(loaded.params[ScopeTrace::INTENSITY_PARAM].getValue(), 1.5f) &&
		near(loaded.params[ScopeTrace::DECAY_PARAM].getValue(), 0.2f) && near(loaded.params[ScopeTrace::TRIG_LEVEL_PARAM].getValue(), -2.f),
		"old patch: the old params load with the same values");
	check(near(loaded.params[ScopeTrace::DWELL_CV_PARAM].getValue(), 1.f) && near(loaded.params[ScopeTrace::TRIG_LEVEL_CV_PARAM].getValue(), 1.f),
		"old patch: the new attenuverters stay at +100%");
	loaded.process(args);
	check(near(loaded.lastScope.dwell, 0.9f) && near(loaded.lastScope.intensity, 1.5f) && near(loaded.lastScope.decay, 0.2f) &&
		near(loaded.lastScope.triggerLevel, -2.f / 5.f), "old patch: the published values equal the saved knobs");

	rack::contextSet(nullptr);
	std::printf("Scope Trace module: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
