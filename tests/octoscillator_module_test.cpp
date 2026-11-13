// Headless Rack-SDK integration check. No GUI or running Rack needed.
#include "../src/Octoscillator.cpp"
#include <cstdio>
Plugin* pluginInstance = nullptr;
int main() {
	int fails = 0, checks = 0;
	auto check = [&](bool v, const char* msg) { ++checks; if (!v) { ++fails; std::fprintf(stderr, "%s\n", msg); } };
	Octoscillator module;
	check(module.outputs.size() == 24, "24 physical mono outputs");
	check(module.inputs.size() == 3, "three CV inputs");
	rack::engine::Module::ProcessArgs args{};
	args.sampleRate = 48000; args.sampleTime = 1.f / 48000;
	for (int i = 0; i < 100; ++i) module.process(args);
	for (auto& output : module.outputs) check(std::isfinite(output.getVoltage()) && std::abs(output.getVoltage()) <= 10.f, "valid voltage");
	json_t* saved = module.dataToJson();
	Octoscillator restored;
	restored.dataFromJson(saved);
	for (int i = 0; i < 8; ++i) {
		check(restored.engine.local[i].x == module.engine.local[i].x, "local JSON restoration");
		check(restored.engine.local[i].y == module.engine.local[i].y, "local Y restoration");
		check(restored.engine.local[i].z == module.engine.local[i].z, "local Z restoration");
	}
	check(restored.engine.shared.x == module.engine.shared.x, "shared JSON restoration");
	json_decref(saved);
	json_t* broken = json_pack("{s:[[s,i,i],[i,i,i]]}", "trajectories", "bad", 1, 20, 0, 0, 0);
	const auto prior = restored.engine.local;
	restored.dataFromJson(broken);
	check(restored.engine.local[0].x == prior[0].x && restored.engine.local[1].x == prior[1].x, "malformed JSON ignored");
	json_decref(broken);
	module.params[Octoscillator::RELATE_PARAM].setValue(1);
	for (int n = 0; n < 64; ++n) module.process(args);
	for (int i = 1; i < 8; ++i) for (int c = 0; c < 3; ++c)
		check(std::abs(module.outputs[i*3+c].getVoltage()-module.outputs[c].getVoltage()) < 1e-6f, "actual output relationship");
	module.inputs[Octoscillator::RESET_INPUT].setVoltage(10);
	module.process(args);
	check(std::abs(module.engine.local[0].x-octoscillator::initialState(0).x)<.1, "reset cable restarts");
	std::printf("Octoscillator module: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
