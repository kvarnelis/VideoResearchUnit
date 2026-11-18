// vru-render-patch: renders what each VRU Display in a .vcv patch would show, without Rack.
//
// It builds the REAL VRU modules (the same Module subclasses the plugin ships, linked from src/*.cpp), loads
// each module's saved params and data with Module::fromJson, connects the patch's cables and runs every
// process() at 48 kHz for the requested time. Each Display's selected input is then drawn with the same
// RenderService the Display uses, and written as PNG frames. Modules from other plugins cannot run, so a
// few stand-ins are provided (Fundamental LFO, VCO, Noise, VCA, 8vert) and everything else outputs 0 V.
// Every stand-in and every unsupported module is listed in the report.
//
// Not part of the plugin dylib. Build with `make build/vru-render-patch` (macOS, needs the Rack SDK).
#include "../src/plugin.hpp"
#include "display_capture.hpp"
#include "display_select.hpp"
#include "video_bus.hpp"
#include "video_external_frame.hpp"
#include "render_service.hpp"
#include "video_graph_renderer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

Plugin* pluginInstance = nullptr;

namespace {

constexpr float kSampleRate = 48000.f;
constexpr double kPi = 3.14159265358979323846;

// ---- Deterministic random numbers ------------------------------------------------------------------------

struct Rng {
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
	uint64_t next() {
		uint64_t z = (s += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}
	double uniform() {  // (0, 1)
		return (static_cast<double>(next() >> 11) + 0.5) / 9007199254740992.0;
	}
	// One gaussian per call, from a cached Box-Muller pair.
	bool have = false;
	double cached = 0.0;
	double normal() {
		if (have) {
			have = false;
			return cached;
		}
		const double r = std::sqrt(-2.0 * std::log(uniform()));
		const double a = 2.0 * kPi * uniform();
		cached = r * std::sin(a);
		have = true;
		return r * std::cos(a);
	}
};

// ---- Fundamental Noise stand-in -----------------------------------------------------------------------------
// White noise is gaussian at every sample. The coloured outputs filter that same stream. Each filter is scaled
// so every output has the same RMS (kNoiseRms volts), measured once at start-up from a separate seeded run.

constexpr double kNoiseRms = 2.0;

struct NoiseState {
	Rng rng;
	double pinkB[7] = {0, 0, 0, 0, 0, 0, 0};
	double red = 0, black = 0, prevWhite = 0, prevPink = 0, grayLow = 0;
	double scale[7] = {1, 1, 1, 1, 1, 1, 1};
	explicit NoiseState(uint64_t seed) : rng(seed) {}
	// Raw (unscaled) values for the seven outputs: white, pink, red, violet, blue, gray, black.
	void step(double out[7]) {
		const double w = rng.normal();
		pinkB[0] = 0.99886 * pinkB[0] + w * 0.0555179;
		pinkB[1] = 0.99332 * pinkB[1] + w * 0.0750759;
		pinkB[2] = 0.96900 * pinkB[2] + w * 0.1538520;
		pinkB[3] = 0.86650 * pinkB[3] + w * 0.3104856;
		pinkB[4] = 0.55000 * pinkB[4] + w * 0.5329522;
		pinkB[5] = -0.7616 * pinkB[5] - w * 0.0168980;
		const double pink = pinkB[0] + pinkB[1] + pinkB[2] + pinkB[3] + pinkB[4] + pinkB[5] + pinkB[6] + w * 0.5362;
		pinkB[6] = w * 0.115926;
		red = 0.9995 * red + w;
		black = 0.9995 * black + red * 0.02;
		const double violet = w - prevWhite;
		const double blue = pink - prevPink;
		grayLow += 0.04 * (w - grayLow);
		const double gray = w + 3.0 * grayLow;
		prevWhite = w;
		prevPink = pink;
		out[0] = w;
		out[1] = pink;
		out[2] = red;
		out[3] = violet;
		out[4] = blue;
		out[5] = gray;
		out[6] = black;
	}
	void calibrate() {
		NoiseState probe(0xC0FFEEull);
		double sum[7] = {0, 0, 0, 0, 0, 0, 0};
		double v[7];
		const int n = 400000, skip = 20000;
		for (int i = 0; i < n + skip; ++i) {
			probe.step(v);
			if (i >= skip) {
				for (int k = 0; k < 7; ++k) {
					sum[k] += v[k] * v[k];
				}
			}
		}
		for (int k = 0; k < 7; ++k) {
			const double rms = std::sqrt(sum[k] / n);
			scale[k] = rms > 1e-12 ? kNoiseRms / rms : 1.0;
		}
	}
};

// ---- Patch graph ---------------------------------------------------------------------------------------------

struct Node {
	int64_t id = 0;
	std::string plugin, model;
	json_t* json = nullptr;
	rack::engine::Module* module = nullptr;  // real module, or null
	// Stand-ins and Displays keep their own port values.
	std::vector<float> in, out;
	std::vector<char> inConnected;
	std::function<void(Node&)> step;  // stand-in per-sample behaviour
	std::string status;               // report line
	bool isDisplay = false;
	int selectedInput = 0;
	// Test card sources (Video Player, Syphon In).
	VideoBusOwner* cardBus = nullptr;
	int cardFrame = 0;
	std::function<float(const Node&, int)> params;  // saved param value or default, for stand-ins
	std::map<int, double> saved;                    // saved params by id (stand-ins)
	std::map<int, double> defaults;
	double param(int id) const {
		auto it = saved.find(id);
		if (it != saved.end()) {
			return it->second;
		}
		auto d = defaults.find(id);
		return d != defaults.end() ? d->second : 0.0;
	}
	float inV(int i) const {
		return module ? module->inputs[i].getVoltage() : in[i];
	}
	bool inUsed(int i) const {
		return module ? module->inputs[i].isConnected() : inConnected[i] != 0;
	}
};

struct PatchCable {
	int64_t outMod, inMod;
	int outId, inId;
};

// 5x7 font (Adafruit glcdfont, public domain), characters 0x20 to 0x5A, five column bytes each, LSB at the top.
const unsigned char kFont[][5] = {
	{0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
	{0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00},
	{0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08},
	{0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
	{0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33},
	{0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
	{0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x00, 0x14, 0x00, 0x00}, {0x00, 0x40, 0x34, 0x00, 0x00},
	{0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06},
	{0x3E, 0x41, 0x5D, 0x59, 0x4E}, {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
	{0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x41, 0x51, 0x73},
	{0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
	{0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
	{0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x26, 0x49, 0x49, 0x49, 0x32},
	{0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
	{0x63, 0x14, 0x08, 0x14, 0x63}, {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43},
};

void drawText(std::vector<unsigned char>& rgba, int w, int h, int x, int y, int scale, const std::string& text, const unsigned char color[3]) {
	for (char raw : text) {
		unsigned char c = static_cast<unsigned char>(raw);
		if (c >= 'a' && c <= 'z') {
			c = static_cast<unsigned char>(c - 32);
		}
		if (c < 0x20 || c > 0x5A) {
			c = '?';
		}
		const unsigned char* g = kFont[c - 0x20];
		for (int col = 0; col < 5; ++col) {
			for (int row = 0; row < 8; ++row) {
				if (!(g[col] & (1 << row))) {
					continue;
				}
				for (int dy = 0; dy < scale; ++dy) {
					for (int dx = 0; dx < scale; ++dx) {
						const int px = x + col * scale + dx, py = y + row * scale + dy;
						if (px >= 0 && px < w && py >= 0 && py < h) {
							unsigned char* p = &rgba[(static_cast<size_t>(py) * w + px) * 4];
							p[0] = color[0], p[1] = color[1], p[2] = color[2], p[3] = 255;
						}
					}
				}
			}
		}
		x += 6 * scale;
	}
}

// A labelled colour-bar test card, 640 x 360. Drawn the same way every run.
std::vector<unsigned char> makeTestCard(const std::string& title, const std::string& line2) {
	const int w = 640, h = 360;
	std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4, 255);
	static const unsigned char bars[7][3] = {{192, 192, 192}, {192, 192, 0}, {0, 192, 192}, {0, 192, 0}, {192, 0, 192}, {192, 0, 0}, {0, 0, 192}};
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			unsigned char* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
			if (y < h * 2 / 3) {
				const unsigned char* b = bars[x * 7 / w];
				p[0] = b[0], p[1] = b[1], p[2] = b[2];
			}
			else {
				const int ramp = x * 255 / (w - 1);
				p[0] = p[1] = p[2] = static_cast<unsigned char>(ramp);
			}
			p[3] = 255;
		}
	}
	const unsigned char white[3] = {255, 255, 255}, black[3] = {0, 0, 0};
	auto centred = [&](int y, int scale, const std::string& t) {
		const int tw = static_cast<int>(t.size()) * 6 * scale;
		const int x = std::max(4, (w - tw) / 2);
		// A black plate behind the text keeps it readable on the bars.
		for (int yy = y - 4; yy < y + 8 * scale + 4; ++yy) {
			for (int xx = x - 6; xx < x + tw + 6; ++xx) {
				if (xx >= 0 && xx < w && yy >= 0 && yy < h) {
					unsigned char* p = &rgba[(static_cast<size_t>(yy) * w + xx) * 4];
					p[0] = p[1] = p[2] = 0;
				}
			}
		}
		drawText(rgba, w, h, x, y, scale, t, white);
	};
	centred(60, 5, "TEST CARD");
	centred(130, 4, title);
	centred(190, 2, line2);
	(void) black;
	return rgba;
}

// ---- Patch reading ----------------------------------------------------------------------------------------

std::string readFile(const std::string& path, bool& ok) {
	std::ifstream f(path, std::ios::binary);
	ok = static_cast<bool>(f);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// A .vcv file is plain JSON here, or a zstd-compressed tar holding patch.json (Rack's own format).
json_t* loadPatch(const std::string& path, std::string& error) {
	bool ok = false;
	std::string text = readFile(path, ok);
	if (!ok) {
		error = "cannot read " + path;
		return nullptr;
	}
	json_error_t je;
	json_t* root = json_loads(text.c_str(), 0, &je);
	if (root) {
		return root;
	}
	// Not plain JSON: try zstd + tar through the shell. Needs zstd and tar on the PATH.
	std::string cmd = "zstd -dc '" + path + "' 2>/dev/null | tar -xOf - patch.json 2>/dev/null";
	FILE* p = popen(cmd.c_str(), "r");
	if (p) {
		std::string out;
		char buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
			out.append(buf, n);
		}
		pclose(p);
		root = json_loads(out.c_str(), 0, &je);
		if (root) {
			return root;
		}
	}
	error = "not a plain JSON patch, and not a zstd tar with patch.json (" + std::string(je.text) + ")";
	return nullptr;
}

struct RealModel {
	const char* slug;
	Model* model;
};

std::vector<RealModel> realModels() {
	return {
		{"DeflectionYoke", modelDeflectionYoke}, {"ScanProcessor", modelScanProcessor}, {"ScopeTrace", modelScopeTrace},
		{"LineField", modelLineField}, {"VideoOsc", modelVideoOsc}, {"Ramps", modelRamps}, {"ColorLab", modelColorLab},
		{"Colorizer", modelColorizer}, {"WaveShaper", modelWaveShaper}, {"Keyer", modelKeyer}, {"BusMixer", modelBusMixer},
		{"Feedback", modelFeedback}, {"FreezeFrame", modelFreezeFrame}, {"DualLfoChaos", modelDualLfoChaos},
		{"OneVoltCvOut", modelOneVoltCvOut}, {"Rotator", modelRotator}, {"Focus", modelFocus}, {"FrameDelay", modelFrameDelay}, {"ScanSync", modelScanSync}, {"Shapes", modelShapes},
		{"VideoMixer", modelVideoMixer}, {"Octoscillator", modelOctoscillator}, {"VectorOsc", modelVectorOsc},
		{"VideoProbe", modelVideoProbe},
	};
}

const char* noiseNames[7] = {"white", "pink", "red", "violet", "blue", "gray", "black"};

std::string hz(double v) {
	char b[64];
	std::snprintf(b, sizeof b, "%.4g Hz", v);
	return b;
}

struct Options {
	std::string patch, outDir;
	double seconds = 2.0;
	int frames = 4;
	int width = 640, height = 480;
	uint64_t seed = 1;
	int fps = 60;
	bool precision = false;  // turn the plugin setting "Full precision between modules" on
	bool hash = false;  // print an FNV-1a hash of every displayed picture, and the render tick times
	int spin = 0;       // busy-spinning background threads, imitating Rack's audio engine threads
	bool threadMode = false;  // drive the render service's thread-mode tick (synchronously) instead of the UI-driven one
};

void usage() {
	std::fprintf(stderr, "usage: vru-render-patch PATCH.vcv --out DIR [--seconds 2] [--frames 4] [--size 640x480] [--seed 1] [--fps 60] [--thread-mode] [--precision]\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto need = [&](const char* name) -> const char* {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s needs a value\n", name);
				return nullptr;
			}
			return argv[++i];
		};
		if (a == "--out") {
			const char* v = need("--out");
			if (!v) return false;
			o.outDir = v;
		}
		else if (a == "--seconds") {
			const char* v = need("--seconds");
			if (!v) return false;
			o.seconds = std::atof(v);
		}
		else if (a == "--frames") {
			const char* v = need("--frames");
			if (!v) return false;
			o.frames = std::atoi(v);
		}
		else if (a == "--size") {
			const char* v = need("--size");
			if (!v || std::sscanf(v, "%dx%d", &o.width, &o.height) != 2) {
				std::fprintf(stderr, "--size wants WIDTHxHEIGHT\n");
				return false;
			}
		}
		else if (a == "--seed") {
			const char* v = need("--seed");
			if (!v) return false;
			o.seed = std::strtoull(v, nullptr, 10);
		}
		else if (a == "--precision") {
			o.precision = true;
		}
		else if (a == "--hash") {
			o.hash = true;
		}
		else if (a == "--spin") {
			const char* v = need("--spin");
			if (!v) return false;
			o.spin = std::atoi(v);
		}
		else if (a == "--thread-mode") {
			o.threadMode = true;
		}
		else if (a == "--fps") {
			const char* v = need("--fps");
			if (!v) return false;
			o.fps = std::atoi(v);
		}
		else if (!a.empty() && a[0] == '-') {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			return false;
		}
		else {
			o.patch = a;
		}
	}
	if (o.patch.empty() || o.outDir.empty() || o.seconds <= 0 || o.frames < 1 || o.width < 8 || o.height < 8 || o.width > 4096 || o.height > 4096 ||
		o.fps < 1) {
		return false;
	}
	return true;
}

void makeDirs(const std::string& path) {
	std::string cur;
	for (size_t i = 0; i <= path.size(); ++i) {
		if (i == path.size() || path[i] == '/') {
			if (!cur.empty()) {
				::mkdir(cur.c_str(), 0755);
			}
		}
		if (i < path.size()) {
			cur += path[i];
		}
	}
}

}  // namespace

int main(int argc, char** argv) {
	Options opt;
	if (!parseArgs(argc, argv, opt)) {
		usage();
		return 2;
	}
	std::string err;
	json_t* patch = loadPatch(opt.patch, err);
	if (!patch) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	makeDirs(opt.outDir);

	// Module::fromJson reaches params through APP->engine, so give the context a real Engine (as the module tests do).
	rack::Context context;
	context.engine = new rack::engine::Engine;
	rack::contextSet(&context);
	Plugin plugin;
	plugin.slug = "VideoResearchUnit";
	std::map<std::string, Model*> real;
	for (const RealModel& rm : realModels()) {
		rm.model->plugin = &plugin;
		rm.model->slug = rm.slug;
		real[rm.slug] = rm.model;
	}

	std::vector<std::unique_ptr<Node>> nodes;
	std::map<int64_t, Node*> byId;
	std::vector<std::string> report;
	std::vector<std::unique_ptr<VideoBusOwner>> cardBuses;
	std::vector<std::string> cardTitles;

	json_t* modulesJ = json_object_get(patch, "modules");
	size_t mi;
	json_t* mj;
	json_array_foreach(modulesJ, mi, mj) {
		auto n = std::make_unique<Node>();
		n->json = mj;
		n->id = json_integer_value(json_object_get(mj, "id"));
		n->plugin = json_string_value(json_object_get(mj, "plugin")) ? json_string_value(json_object_get(mj, "plugin")) : "";
		n->model = json_string_value(json_object_get(mj, "model")) ? json_string_value(json_object_get(mj, "model")) : "";
		if (json_t* pj = json_object_get(mj, "params")) {
			size_t pi;
			json_t* pe;
			json_array_foreach(pj, pi, pe) {
				n->saved[static_cast<int>(json_integer_value(json_object_get(pe, "id")))] = json_number_value(json_object_get(pe, "value"));
			}
		}
		const std::string name = n->plugin + "/" + n->model + " #" + std::to_string(n->id);
		const bool isVru = n->plugin == "VideoResearchUnit";
		n->in.assign(64, 0.f);
		n->out.assign(64, 0.f);
		n->inConnected.assign(64, 0);

		if (isVru && n->model == "Display") {
			n->isDisplay = true;
			if (json_t* dj = json_object_get(mj, "data")) {
				n->selectedInput = displayClampInput(static_cast<int>(json_integer_value(json_object_get(dj, "selectedInput"))));
			}
			n->status = name + ": Display (rendered by the tool, selected input " + std::string(1, 'A' + n->selectedInput) + ")";
		}
		else if (isVru && real.count(n->model)) {
			n->module = real[n->model]->createModule();
			// Module::fromJson would look the model up in Rack's plugin list, which does not exist here, so do the
			// two parts that matter by hand: the saved params, then the module's own data.
			if (json_t* pj = json_object_get(mj, "params")) {
				n->module->paramsFromJson(pj);
			}
			if (json_t* dj = json_object_get(mj, "data")) {
				n->module->dataFromJson(dj);
			}
			else if (n->model == "Feedback") {
				// Rack's Module::fromJson reaches Feedback::fromJson, which turns the fade option off for saved
				// modules; a saved Feedback with no data object is read as an empty one to give the same result.
				json_t* empty = json_object();
				n->module->dataFromJson(empty);
				json_decref(empty);
			}
			rack::engine::Module::SampleRateChangeEvent e;
			e.sampleRate = kSampleRate;
			e.sampleTime = 1.f / kSampleRate;
			n->module->onSampleRateChange(e);
			n->status = name + ": real module";
			if (n->model == "VideoProbe") {
				n->status += " (its picture sampler only updates while Rack draws the widget, so its outputs stay 0 V)";
			}
		}
		else if (isVru && (n->model == "VideoPlayer" || n->model == "SyphonIn")) {
			const std::string title = n->model == "VideoPlayer" ? "VIDEO PLAYER" : "SYPHON IN";
			auto bus = std::make_unique<VideoBusOwner>();
			json_t* dj = json_object_get(mj, "data");
			if (dj) {
				if (json_t* idj = json_object_get(dj, "busId")) {
					if (json_is_integer(idj)) {
						bus->claimPreferred(static_cast<int>(json_integer_value(idj)));
					}
				}
				else if (json_t* vj = json_object_get(dj, "outputBusVoltage")) {
					if (json_is_number(vj)) {
						bus->claimPreferred(videoBusIdFromVoltage(static_cast<float>(json_number_value(vj))));
					}
				}
			}
			const int frameId = videoExternalFrameAllocate();
			std::vector<unsigned char> card = makeTestCard(title, "NO SOURCE OFFLINE");
			videoExternalFramePublish(frameId, card.data(), 640, 360, 0.0);
			VideoBusOwner* raw = bus.get();
			cardBuses.push_back(std::move(bus));
			n->step = [raw, frameId](Node& self) {
				videoBusPublishExternalFrame(raw->id(), frameId);
				self.out[0] = raw->voltage();
			};
			n->status = name + ": no source offline, draws a labelled test card on bus " + std::to_string(raw->id());
		}
		else if (n->plugin == "Fundamental" && n->model == "LFO") {
			n->defaults = {{0, 1}, {1, 0}, {2, 1}, {4, 0.5}};
			const bool uni = n->param(0) > 0.0, inv = n->param(1) > 0.0;
			const double f = std::pow(2.0, n->param(2));
			const double pw = std::min(0.99, std::max(0.01, n->param(4)));
			auto phase = std::make_shared<double>(0.0);
			n->step = [uni, inv, f, pw, phase](Node& self) {
				const double p = *phase;
				double sine = std::sin(2.0 * kPi * p);
				double tri = p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p;
				double saw = 2.0 * p - 1.0;
				double sqr = p < pw ? 1.0 : -1.0;
				const double v[4] = {sine, tri, saw, sqr};
				for (int k = 0; k < 4; ++k) {
					double o = 5.0 * v[k] * (inv ? -1.0 : 1.0);
					self.out[k] = static_cast<float>(uni ? o + 5.0 : o);
				}
				*phase = std::fmod(p + f / kSampleRate, 1.0);
			};
			n->status = name + ": stand-in, sine/triangle/saw/square at " + hz(f) + (uni ? ", 0 to 10 V" : ", +-5 V") + (inv ? ", inverted" : "") +
				", saved FREQ read; CV inputs ignored, starts at phase 0";
		}
		else if (n->plugin == "Fundamental" && n->model == "VCO") {
			n->defaults = {{2, 0}, {3, 0}, {5, 0.5}};
			const double semis = n->param(2) + 3.0 * n->param(3) * std::fabs(n->param(3));  // FINE uses a quadratic taper
			const double f0 = 261.6256 * std::pow(2.0, semis / 12.0);
			const double pw = std::min(0.99, std::max(0.01, n->param(5)));
			auto phase = std::make_shared<double>(0.0);
			n->step = [f0, pw, phase](Node& self) {
				// 1 V/oct on input 0 (PITCH) is the one input read; FM, SYNC and PW inputs are ignored.
				const double f = f0 * std::pow(2.0, static_cast<double>(self.inV(0)));
				const double p = *phase;
				const double sine = std::sin(2.0 * kPi * p);
				const double tri = p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p;
				const double saw = 2.0 * p - 1.0;
				const double sqr = p < pw ? 1.0 : -1.0;
				self.out[0] = static_cast<float>(5.0 * sine);
				self.out[1] = static_cast<float>(5.0 * tri);
				self.out[2] = static_cast<float>(5.0 * saw);
				self.out[3] = static_cast<float>(5.0 * sqr);
				*phase = std::fmod(p + f / kSampleRate, 1.0);
			};
			n->status = name + ": stand-in, sine/triangle/saw/square at " + hz(f0) + " (+-5 V), saved FREQ and FINE read, PITCH input followed at 1 V/oct; FM, SYNC and PW inputs ignored, plain (not band-limited) waves";
		}
		else if (n->plugin == "Fundamental" && n->model == "Noise") {
			auto state = std::make_shared<NoiseState>(opt.seed * 1000003ull + static_cast<uint64_t>(n->id));
			state->calibrate();
			n->step = [state](Node& self) {
				double v[7];
				state->step(v);
				for (int k = 0; k < 7; ++k) {
					self.out[k] = static_cast<float>(std::max(-5.0, std::min(5.0, v[k] * state->scale[k])));
				}
			};
			n->status = name + ": stand-in, new seeded gaussian noise at every sample of 48 kHz; outputs white, pink, red, violet, blue, gray, black; "
				"each at about 2 V RMS limited to +-5 V (levels assumed); gray and black are approximations of the real curves";
		}
		else if (n->plugin == "Fundamental" && n->model == "VCA") {
			n->defaults = {{0, 1}, {1, 1}};
			const double l0 = n->param(0), l1 = n->param(1);
			n->step = [l0, l1](Node& self) {
				for (int ch = 0; ch < 2; ++ch) {
					const int base = ch * 3;  // EXP, LIN, IN
					double g = ch == 0 ? l0 : l1;
					if (self.inUsed(base + 1)) {
						g *= std::min(1.0, std::max(0.0, self.inV(base + 1) / 10.0));
					}
					if (self.inUsed(base)) {
						const double e = std::min(1.0, std::max(0.0, self.inV(base) / 10.0));
						g *= e * e * e * e;
					}
					self.out[ch] = static_cast<float>(self.inV(base + 2) * g);
				}
			};
			n->status = name + ": stand-in, output = IN x LEVEL x LIN CV (EXP CV to the 4th power); port order assumed";
		}
		else if (n->plugin == "Fundamental" && n->model == "8vert") {
			n->defaults = {};
			n->step = [](Node& self) {
				// Each input is normalled to the one above, the first to 10 V. The knob scales it.
				float above = 10.f;
				for (int c = 0; c < 8; ++c) {
					const float v = self.inUsed(c) ? self.inV(c) : above;
					above = v;
					self.out[c] = v * static_cast<float>(self.param(c));
				}
			};
			n->status = name + ": stand-in, each input (normalled down from 10 V) times its saved knob";
		}
		else if (n->plugin == "Core") {
			n->status = name + ": Rack's own module, ignored (no signal offline)";
		}
		else {
			n->status = name + ": unsupported, outputs 0 V";
		}
		byId[n->id] = n.get();
		nodes.push_back(std::move(n));
	}

	// Cables.
	std::vector<PatchCable> cables;
	json_t* cablesJ = json_object_get(patch, "cables");
	size_t ci;
	json_t* cj;
	json_array_foreach(cablesJ, ci, cj) {
		PatchCable c;
		c.outMod = json_integer_value(json_object_get(cj, "outputModuleId"));
		c.inMod = json_integer_value(json_object_get(cj, "inputModuleId"));
		c.outId = static_cast<int>(json_integer_value(json_object_get(cj, "outputId")));
		c.inId = static_cast<int>(json_integer_value(json_object_get(cj, "inputId")));
		auto o = byId.find(c.outMod), i = byId.find(c.inMod);
		if (o == byId.end() || i == byId.end()) {
			report.push_back("cable " + std::to_string(ci + 1) + ": names a module that is not in the patch, ignored");
			continue;
		}
		Node *on = o->second, *in = i->second;
		const bool outOk = on->module ? c.outId >= 0 && c.outId < static_cast<int>(on->module->outputs.size()) : c.outId >= 0 && c.outId < 64;
		const bool inOk = in->module ? c.inId >= 0 && c.inId < static_cast<int>(in->module->inputs.size()) : c.inId >= 0 && c.inId < 64;
		if (!outOk || !inOk) {
			report.push_back("cable " + std::to_string(ci + 1) + ": port id out of range, ignored");
			continue;
		}
		if (on->module) {
			on->module->outputs[c.outId].channels = 1;  // setChannels() does nothing on a port with no cable
		}
		if (in->module) {
			in->module->inputs[c.inId].channels = 1;
		}
		else {
			in->inConnected[c.inId] = 1;
		}
		cables.push_back(c);
	}
	// Inputs of stand-ins that are patched but not read are named in the report.
	for (auto& np : nodes) {
		Node& n = *np;
		if (!n.module && !n.isDisplay && n.step && n.plugin == "Fundamental") {
			std::string ign;
			for (const PatchCable& c : cables) {
				if (c.inMod == n.id) {
					const bool read = (n.model == "VCO" && c.inId == 0) || (n.model == "VCA") || (n.model == "8vert");
					if (!read) {
						ign += (ign.empty() ? "" : ", ") + std::to_string(c.inId);
					}
				}
			}
			if (!ign.empty()) {
				n.status += " [patched input ids not read: " + ign + "]";
			}
		}
	}

	// Displays.
	struct DisplayRun {
		Node* node;
		int frames = 0;
		bool everSignal = false;
	};
	std::vector<DisplayRun> displays;
	for (auto& np : nodes) {
		if (np->isDisplay) {
			DisplayRun d;
			d.node = np.get();
			displays.push_back(std::move(d));
		}
	}

	// Displays draw through the plugin's own render service, the way the real Display widget does. One render
	// round is one UI frame, so the tick counter moves once per round and Displays on the same bus and size
	// share one picture.
	static uint64_t renderTick = 0;
	static double renderTime = 0.0;
	renderService().clear();
	renderService().setFullPrecision(opt.precision);
	renderService().setTickSource([]() -> uint64_t { return renderTick; });
	if (opt.threadMode) {
		// The render thread's tick, run synchronously: every round, each Display registers its bus (what step() does),
		// one thread tick renders everything registered in one pass per size, and the Display reads the finished picture.
		renderService().setTimeSource([]() -> double { return renderTime; });
		renderService().setThreadAutoStart(false);
		renderService().setThreadMode(true);
	}

	rack::engine::Module::ProcessArgs args{};
	args.sampleRate = kSampleRate;
	args.sampleTime = 1.f / kSampleRate;
	const long total = std::max(1L, std::lround(opt.seconds * kSampleRate));
	const long renderEvery = std::max(1L, std::lround(kSampleRate / opt.fps));
	std::vector<long> captureAt;
	for (int k = 0; k < opt.frames; ++k) {
		captureAt.push_back(std::max(1L, std::lround(total * static_cast<double>(k + 1) / opt.frames)));
	}
	size_t nextCapture = 0;
	std::vector<double> tickMs;
	std::atomic<bool> stopSpin{false};
	std::vector<std::thread> spinners;
	for (int k = 0; k < opt.spin; ++k) {
		spinners.emplace_back([&stopSpin] {
			volatile uint64_t x = 0;
			while (!stopSpin.load(std::memory_order_relaxed)) {
				x = x + 1;
			}
		});
	}
	int written = 0;

	auto outVoltage = [](Node& n, int id) -> float { return n.module ? n.module->outputs[id].getVoltage() : n.out[id]; };

	for (long s = 1; s <= total; ++s) {
		args.frame = s - 1;  // Rack's frame counter; the scan modules stamp their signal rings with it (src/video_clock.hpp)
		for (auto& np : nodes) {
			Node& n = *np;
			if (n.module) {
				n.module->process(args);
			}
			else if (n.step) {
				n.step(n);
			}
		}
		for (const PatchCable& c : cables) {
			Node* on = byId[c.outMod];
			Node* in = byId[c.inMod];
			const float v = outVoltage(*on, c.outId);
			if (in->module) {
				in->module->inputs[c.inId].setVoltage(v);
			}
			else {
				in->in[c.inId] = v;
			}
		}
		const bool capture = nextCapture < captureAt.size() && s == captureAt[nextCapture];
		if (capture || s % renderEvery == 0) {
			++renderTick;
			renderTime = static_cast<double>(s) / kSampleRate;
			auto busOf = [](DisplayRun& d) {
				const int input = d.node->selectedInput;
				const bool connected = d.node->inConnected[input] != 0;
				return displayInputBusId(input, connected, connected ? d.node->in[input] : 0.f);
			};
			if (opt.threadMode) {
				for (DisplayRun& d : displays) {
					renderService().touch(&d, busOf(d), opt.width, opt.height);
				}
				const auto tick0 = std::chrono::steady_clock::now();
				renderService().runTickForTest();
				tickMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tick0).count());
			}
			for (DisplayRun& d : displays) {
				const int busId = busOf(d);
				const RenderService::Result picture = opt.threadMode ? renderService().frame(&d, busId, opt.width, opt.height) : renderService().frame(busId, opt.width, opt.height);
				const RenderService::Frame& frame = *picture.pixels;
				const bool hasPicture = picture.signal && frame.size() == static_cast<size_t>(opt.width) * opt.height * 4u;
				d.everSignal = d.everSignal || hasPicture;
				if (opt.hash && hasPicture) {
					uint64_t h = 1469598103934665603ull;
					for (unsigned char b : frame) {
						h = (h ^ b) * 1099511628211ull;
					}
					std::printf("HASH %llu %016llx\n", static_cast<unsigned long long>(renderTick), static_cast<unsigned long long>(h));
				}
				if (capture && hasPicture) {
					std::vector<unsigned char> png;
					std::string perr;
					if (display_capture::encodePng(frame.data(), opt.width, opt.height, png, perr)) {
						const int ms = static_cast<int>(std::lround(1000.0 * s / kSampleRate));
						char name[128];
						std::snprintf(name, sizeof name, "display-%lld-frame-%02d-%05dms.png", static_cast<long long>(d.node->id), d.frames + 1, ms);
						const std::string path = opt.outDir + "/" + name;
						FILE* f = std::fopen(path.c_str(), "wb");
						if (f) {
							std::fwrite(png.data(), 1, png.size(), f);
							std::fclose(f);
							++written;
							++d.frames;
						}
					}
				}
			}
			if (capture) {
				++nextCapture;
			}
		}
	}

	stopSpin = true;
	for (std::thread& t : spinners) {
		t.join();
	}
	if (opt.hash && !tickMs.empty()) {
		std::vector<double> v = tickMs;
		std::sort(v.begin(), v.end());
		double sum = 0;
		for (double x : v) sum += x;
		std::printf("TICK n=%zu mean %.2f median %.2f p95 %.2f max %.2f ms\n", v.size(), sum / v.size(), v[v.size() / 2], v[v.size() * 95 / 100], v.back());
	}
	// Report.
	std::vector<std::string> lines;
	char head[256];
	std::snprintf(head, sizeof head, "vru-render-patch: %s, %.4g s at 48 kHz, %d frames per Display, %dx%d, seed %llu, %d renders per second",
		opt.patch.c_str(), opt.seconds, opt.frames, opt.width, opt.height, static_cast<unsigned long long>(opt.seed), opt.fps);
	lines.push_back(head);
	lines.push_back("modules:");
	for (auto& np : nodes) {
		lines.push_back("  " + np->status);
	}
	for (const std::string& r : report) {
		lines.push_back("  " + r);
	}
	lines.push_back("displays:");
	for (DisplayRun& d : displays) {
		const int input = d.node->selectedInput;
		std::string l = "  Display #" + std::to_string(d.node->id) + " input " + std::string(1, 'A' + input) + ": ";
		l += d.frames > 0 ? std::to_string(d.frames) + " PNG frames written" : "no video on this input, nothing written";
		lines.push_back(l);
	}
	lines.push_back("PNG files written: " + std::to_string(written));
	std::string text;
	for (const std::string& l : lines) {
		text += l + "\n";
	}
	std::fputs(text.c_str(), stdout);
	{
		std::ofstream rf(opt.outDir + "/report.txt");
		rf << text;
	}

	for (auto& np : nodes) {
		delete np->module;
	}
	cardBuses.clear();
	json_decref(patch);
	return 0;
}
