// Scan Sync (VRU-28) in an engine test: the real ScanSync and DeflectionYoke modules, driven sample by sample the way
// Rack drives them (ProcessArgs::frame counting up), with a cable from a Scan Sync output to the Yoke's H coil input
// (audio rate). The picture the Yoke draws must be steady from field to field, at 44.1, 48 and 96 kHz, which is
// the point of the module. A free-running ramp at 60.000 Hz is the control: it creeps. Needs the macOS Rack SDK for
// Module, Port and Param; no running Rack.
#include "../src/ScanSync.cpp"
#include "../src/DeflectionYoke.cpp"

#include "video_external_frame.hpp"
#include "video_graph_renderer.hpp"

#include <cstdio>
#include <vector>

Plugin* pluginInstance = nullptr;

namespace {

int fails = 0;
int checks = 0;
void check(bool ok, const char* msg) {
	++checks;
	if (!ok) {
		++fails;
		std::fprintf(stderr, "FAIL: %s\n", msg);
	}
}

using Frame = std::vector<unsigned char>;
constexpr int kW = 192;
constexpr int kH = 240;

Frame lineSource() {
	Frame f(static_cast<size_t>(kW) * kH * 4u, 0);
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const size_t i = (static_cast<size_t>(y) * kW + x) * 4u;
			f[i] = f[i + 1] = f[i + 2] = x == kW / 2 ? 255 : 0;
			f[i + 3] = 255;
		}
	}
	return f;
}

// Row centres of the lit line, in pixels, NaN where the row is dark.
std::vector<double> centroids(const Frame& f) {
	std::vector<double> c(static_cast<size_t>(kH), std::nan(""));
	std::vector<double> sums(static_cast<size_t>(kH), 0.0);
	double top = 0.0;
	for (int y = 0; y < kH; ++y) {
		double sum = 0.0;
		double weighted = 0.0;
		for (int x = 0; x < kW; ++x) {
			const size_t i = (static_cast<size_t>(y) * kW + x) * 4u;
			const double luma = (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
			sum += luma;
			weighted += luma * (x + 0.5);
		}
		sums[static_cast<size_t>(y)] = sum;
		top = std::max(top, sum);
		if (sum > 0.0) {
			c[static_cast<size_t>(y)] = weighted / sum;
		}
	}
	for (int y = 0; y < kH; ++y) {
		if (!(sums[static_cast<size_t>(y)] >= 0.5 * top)) {
			c[static_cast<size_t>(y)] = std::nan("");
		}
	}
	return c;
}

// Worst difference between two pictures' row centres from row `fromRow` down, and how many rows were compared.
double worstDiff(const Frame& a, const Frame& b, int fromRow, int* rows) {
	const std::vector<double> ca = centroids(a);
	const std::vector<double> cb = centroids(b);
	double worst = 0.0;
	int n = 0;
	for (int y = fromRow; y < kH; ++y) {
		const size_t i = static_cast<size_t>(y);
		if (!std::isnan(ca[i]) && !std::isnan(cb[i])) {
			worst = std::max(worst, std::fabs(ca[i] - cb[i]));
			++n;
		}
	}
	*rows = n;
	return worst;
}

// Drives ScanSync (or a free-running 60.000 Hz ramp) into a Yoke's H coil input, and renders the Yoke's ring after
// each of the fields in `renderFields`. Returns the pictures.
std::vector<Frame> run(double rate, bool useScanSync, const std::vector<int>& renderFields) {
	ScanSync sync;
	DeflectionYoke yoke;
	yoke.params[DeflectionYoke::X_BEND_PARAM].setValue(0.4f);
	yoke.params[DeflectionYoke::Y_BEND_PARAM].setValue(0.f);
	yoke.params[DeflectionYoke::S_BEND_PARAM].setValue(0.f);
	yoke.params[DeflectionYoke::ZOOM_PARAM].setValue(0.f);
	yoke.params[DeflectionYoke::DEPTH_PARAM].setValue(1.f);

	const int frameId = videoExternalFrameAllocate();
	const Frame src = lineSource();
	videoExternalFramePublish(frameId, src.data(), kW, kH, 0.0);
	videoBusPublishExternalFrame(40, frameId);

	rack::engine::Module::ProcessArgs args{};
	args.sampleRate = static_cast<float>(rate);
	args.sampleTime = static_cast<float>(1.0 / rate);

	const vclock::Clock clock(rate, 240);
	std::vector<Frame> pictures;
	VideoGraphRenderer renderer;
	size_t next = 0;
	const int64_t last = static_cast<int64_t>(std::floor((renderFields.back() + 1) * clock.samplesPerField())) + 8;
	for (int64_t f = 0; f <= last; ++f) {
		args.frame = f;
		float volts = 0.f;
		if (useScanSync) {
			sync.process(args);
			volts = sync.outputs[ScanSync::DOWN_OUTPUT].getVoltage();
		}
		else {
			volts = static_cast<float>(10.0 * std::fmod(60.0 * static_cast<double>(f) / rate, 1.0));  // a saw at exactly 60 Hz
		}
		yoke.inputs[DeflectionYoke::X_BEND_INPUT].channels = 1;  // a cable is patched (setChannels does nothing on a disconnected port)
		yoke.inputs[DeflectionYoke::X_BEND_INPUT].setVoltage(volts);
		yoke.process(args);
		if (next < renderFields.size() && f == static_cast<int64_t>(std::floor((renderFields[next] + 1) * clock.samplesPerField())) + 5) {
			DeflectionYokeParams p = yoke.wobble;
			p.signalSlotId = yoke.signalSlotId;
			p.persist = 0.f;  // no phosphor persistence, so each picture is its own field's
			videoBusPublishDeflectionYoke(41, 40, p);
			pictures.push_back(renderer.renderBus(41, kW, kH));
			++next;
		}
	}
	videoBusClear(41);
	videoBusClear(40);
	videoExternalFrameRelease(frameId);
	return pictures;
}

}  // namespace

int main() {
	rack::Context context;
	context.engine = new rack::engine::Engine;
	rack::contextSet(&context);

	// The module itself: outputs follow the clock.
	{
		ScanSync sync;
		check(sync.params.size() == 2 && sync.outputs.size() == 4 && sync.inputs.size() == 0, "Scan Sync has 2 params (LINES, SLOW), no inputs and 4 outputs");
		rack::engine::Module::ProcessArgs args{};
		args.sampleRate = 48000.f;
		args.sampleTime = 1.f / 48000.f;
		int pictures = 0;
		float peak = 0.f;
		bool inRange = true;
		for (int64_t f = 0; f < 48000; ++f) {
			args.frame = f;
			sync.process(args);
			const float top = sync.outputs[ScanSync::TOP_OUTPUT].getVoltage();
			static float previous = 0.f;
			pictures += top > 5.f && previous < 5.f;
			previous = top;
			for (int o = 0; o < 4; ++o) {
				const float v = sync.outputs[o].getVoltage();
				inRange = inRange && v >= 0.f && v <= 10.f;
				peak = std::max(peak, v);
			}
		}
		check(pictures == 60 || pictures == 59, "about 59.94 top pulses in a second");
		check(inRange && peak == 10.f, "every output stays within 0 to 10 V and reaches 10 V");
		sync.params[ScanSync::LINES_PARAM].setValue(120.f);
		int p120 = 0;
		float prev = 0.f;
		for (int64_t f = 48000; f < 96000; ++f) {
			args.frame = f;
			sync.process(args);
			const float top = sync.outputs[ScanSync::TOP_OUTPUT].getVoltage();
			p120 += top > 5.f && prev < 5.f;
			prev = top;
		}
		std::printf("  p120 %d\n", p120);
		check(p120 >= 109 && p120 <= 111, "LINES 120 gives about 110 pictures a second");
	}

	// The point of the module: ordinary Rack signals driven by its ramp draw a steady picture.
	std::vector<int> fields;
	for (int k = 60; k < 68; ++k) {
		fields.push_back(k);
	}
	for (int k = 100; k <= 660; k += 40) {  // out to 660 fields: a drift of a tenth of a cycle per 60 fields would show
		fields.push_back(k);
	}
	for (double rate : {44100.0, 48000.0, 96000.0}) {
		const std::vector<Frame> steady = run(rate, true, fields);
		const std::vector<Frame> creeping = run(rate, false, fields);
		check(steady.size() == fields.size() && creeping.size() == fields.size(), "every field was drawn");
		if (std::getenv("VRU_DEBUG")) {
			for (int i = 0; i < 3; ++i) {
				const std::vector<double> c0 = centroids(steady[static_cast<size_t>(i)]);
				std::printf("  field %d:", fields[static_cast<size_t>(i)]);
				for (int y = 0; y < kH; y += 24) std::printf(" %.2f", c0[static_cast<size_t>(y)]);
				std::printf("\n");
			}
		}
		double worstSteady = 0.0;
		double worstCreep = 0.0;
		int rows = 0;
		int rowsCreep = 0;
		for (size_t i = 1; i < steady.size(); ++i) {
			// Skip the top 40 rows. There the ramp restarts, and the Yoke's coil filter (3 kHz, about five lines of
			// smoothing) spreads the restart over a distance that depends on where it falls between two samples (a
			// third of a line per sample). That is the sampling of the signal, not drift; by row 40 it is gone.
			if (std::getenv("VRU_DEBUG")) {
				std::printf("   field %d: %.4f (rows 40+), %.4f (rows 100+)\n", fields[i], worstDiff(steady[0], steady[i], 40, &rows), worstDiff(steady[0], steady[i], 100, &rows));
			}
			worstSteady = std::max(worstSteady, worstDiff(steady[0], steady[i], 40, &rows));
			worstCreep = std::max(worstCreep, worstDiff(creeping[0], creeping[i], 40, &rowsCreep));
		}
		std::printf("  %.0f Hz: ramp from Scan Sync, worst row shift out to field %zu %.4f px (%d rows); free-running 60.000 Hz saw %.2f px\n",
			rate, static_cast<size_t>(fields.back()), worstSteady, rows, worstCreep);
		check(rows > kH / 2, "most rows are lit and compared");
		check(worstSteady < 0.02, "Scan Sync's ramp draws the same picture field after field");
		check(worstCreep > 5.0, "control: a free-running 60 Hz saw does not");
	}

	std::printf("scan_sync_module_test: %d checks, %d failed\n", checks, fails);
	return fails == 0 ? 0 : 1;
}
