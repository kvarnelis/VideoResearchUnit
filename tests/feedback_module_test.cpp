// Headless Feedback module check against Rack's real Module/JSON code. No GUI or running Rack.
// The "Fade trails fully to black" option: on for a new module and after Initialize, saved in the module's JSON,
// off for any saved data without the key (every patch saved before the option), and carried to the renderer in the
// node's params. Rack calls Module::fromJson for each module it loads from a patch and never for a new one.
#include "../src/Feedback.cpp"
#include <cstdio>

Plugin* pluginInstance = nullptr;

// Rack sets a module's model when it creates the module; here the test does.
struct F : Feedback {
	F() { model = modelFeedback; }
};

int main() {
	int fails = 0, checks = 0;
	auto check = [&](bool v, const char* msg) { ++checks; if (!v) { ++fails; std::fprintf(stderr, "FAIL: %s\n", msg); } };
	rack::engine::Module::ProcessArgs args{};
	args.sampleRate = 48000.f;
	args.sampleTime = 1.f / 48000.f;

	rack::Context context;
	context.engine = new rack::engine::Engine;
	rack::contextSet(&context);
	Plugin plugin;
	plugin.slug = "VideoResearchUnit";
	modelFeedback->plugin = &plugin;
	modelFeedback->slug = "Feedback";
	// Module::fromJson looks its model up in Rack's plugin list, so list this one.
	plugin.models.push_back(modelFeedback);
	rack::plugin::plugins.push_back(&plugin);

	// A video source for the module to read, so process() publishes a node.
	VideoBusOwner source;
	ShapesParams shape;
	videoBusPublishShapes(source.id(), shape);
	auto publishedFade = [&](Feedback& m) {
		m.inputs[Feedback::VIDEO_BUS_INPUT].setVoltage(source.voltage());
		m.process(args);
		VideoBusNode node;
		if (!videoBusReadNode(m.videoBus.id(), node) || node.type != VideoBusNodeType::Feedback) return -1;
		return node.feedback.fadeToBlack;
	};

	// A new module has the option on, and says so on the bus.
	{
		F fresh;
		check(fresh.fadeToBlack.load(), "a new module has the option on");
		check(publishedFade(fresh) == 1, "a new module publishes fadeToBlack = 1");

		// It is saved under its own key.
		json_t* saved = fresh.dataToJson();
		json_t* key = json_object_get(saved, "fadeTrailsToBlack");
		check(key && json_is_true(key), "the option is saved as true");
		json_decref(saved);

		// Turned off, it saves false and loads false; turned on, true.
		fresh.fadeToBlack.store(false);
		saved = fresh.dataToJson();
		key = json_object_get(saved, "fadeTrailsToBlack");
		check(key && json_is_false(key), "the option is saved as false");
		F loaded;
		loaded.dataFromJson(saved);
		check(!loaded.fadeToBlack.load(), "saved false loads false");
		check(publishedFade(loaded) == 0, "off publishes fadeToBlack = 0");
		json_decref(saved);
		fresh.fadeToBlack.store(true);
		saved = fresh.dataToJson();
		loaded.dataFromJson(saved);
		check(loaded.fadeToBlack.load(), "saved true loads true");
		json_decref(saved);
	}

	// Saved data from before the option: a bus id and no key. It loads with the option off.
	{
		json_t* old = json_pack("{s:i}", "busId", 5);
		F m;
		check(m.fadeToBlack.load(), "constructed on");
		m.dataFromJson(old);
		check(!m.fadeToBlack.load(), "data without the key loads with the option off");
		json_decref(old);
		json_t* legacy = json_pack("{s:f}", "outputBusVoltage", 0.5);
		F n;
		n.dataFromJson(legacy);
		check(!n.fadeToBlack.load(), "legacy bus-voltage data loads with the option off");
		json_decref(legacy);
		json_t* empty = json_object();
		F o;
		o.dataFromJson(empty);
		check(!o.fadeToBlack.load(), "empty data loads with the option off");
		json_decref(empty);
	}

	// Rack's own Module::fromJson: a patch module with no data object at all is off; one with the key follows it.
	{
		F fresh;
		json_t* whole = fresh.toJson();
		json_object_del(whole, "data");
		F m;
		m.fromJson(whole);
		check(!m.fadeToBlack.load(), "a patch module with no data object loads with the option off");
		json_decref(whole);

		fresh.fadeToBlack.store(true);
		whole = fresh.toJson();
		F n;
		n.fadeToBlack.store(false);
		n.fromJson(whole);
		check(n.fadeToBlack.load(), "a patch module saved with the option on loads on");
		json_decref(whole);

		// A preset or patch whose data lacks the key turns it off, whatever the module had before.
		json_t* noKey = fresh.toJson();
		json_object_del(json_object_get(noKey, "data"), "fadeTrailsToBlack");
		F p;
		p.fadeToBlack.store(true);
		p.fromJson(noKey);
		check(!p.fadeToBlack.load(), "loading data without the key turns the option off");
		json_decref(noKey);
	}

	// Initialize turns it on, from either state.
	{
		F m;
		m.fadeToBlack.store(false);
		m.onReset(rack::engine::Module::ResetEvent());
		check(m.fadeToBlack.load(), "Initialize turns the option on");
	}

	std::printf("Feedback module: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
