// Picture as modulation (docs/architecture.md, "Picture as modulation").
//
// A module with a second video bus input lets the brightness of that picture, at each pixel, offset one of its
// controls at that pixel. This file checks the four modules that take one: Video Oscillator (FM IN), Ramps,
// Keyer and Color Lab (MOD IN).
//
// The first section pins what the four modules draw with nothing patched, taken on the code as it was before
// the modulation inputs existed. Those pictures must never change. Run with VRU_PRINT_GOLDEN=1 to print hashes.
// Needs no Rack SDK.

#include "video_bus.hpp"
#include "video_graph_renderer.hpp"
#include "render_service.hpp"
#include "frame_ring.hpp"
#include "video_external_frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using Frame = std::vector<unsigned char>;
constexpr int kW = 320;
constexpr int kH = 180;

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL: %s\n", what);
	}
}

uint64_t hashFrame(const Frame& f) {
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : f) {
		h = (h ^ c) * 1099511628211ull;
	}
	return h;
}

bool printGolden() {
	static const bool on = std::getenv("VRU_PRINT_GOLDEN") != nullptr;
	return on;
}

void golden(const char* name, const Frame& f, uint64_t expected) {
	const uint64_t h = hashFrame(f);
	if (printGolden()) {
		std::printf("  GOLDEN %s %lluull\n", name, static_cast<unsigned long long>(h));
	}
	char text[160];
	std::snprintf(text, sizeof(text), "%s hash is pinned", name);
	check(h == expected, text);
}

void clearBuses() {
	for (int bus = 1; bus <= kVideoBusCount; ++bus) {
		videoBusClear(bus);
	}
}

// ---- The scenes. Bus numbers are fixed so a later section can patch a modulation bus into the same scene. ----
constexpr int kOsc = 1;
constexpr int kRamp = 2;
constexpr int kKeyer = 3;
constexpr int kColorLab = 4;

VideoParams barsParams(bool barsA) {
	VideoParams p;
	p.pattern = static_cast<int>(VideoPattern::BarsAndRows);
	p.hue = 0.3f;
	p.feedback = 0.f;
	p.shape = 0.f;
	p.cyclesA = barsA ? 20.f : 6.f;
	p.cyclesB = 3.f;
	p.barsA = barsA;
	p.barsB = false;
	p.mix = 1;
	p.lockA = 1.f;
	p.lockB = 1.f;
	return p;
}

VideoParams plainOscParams() {
	VideoParams p;
	p.pattern = static_cast<int>(VideoPattern::Oscillator);
	p.frequency = 5.f;
	p.feedback = 0.f;
	return p;
}

RampsParams rampParams() {
	RampsParams p;
	p.axisA = 0;      // horizontal: a left-to-right ramp
	p.freqA = 2.f;
	p.phaseA = 0.1f;
	p.waveA = 0.f;
	p.axisB = 1;      // vertical
	p.freqB = 1.f;
	p.mix = 1;
	return p;
}

KeyerParams keyerParams() {
	KeyerParams k;
	k.threshold = 0.5f;
	k.softness = 0.2f;
	k.mode = 2;
	return k;
}

ColorLabParams colorLabParams() {
	ColorLabParams c;
	c.hue = 0.1f;
	c.saturation = 1.2f;
	return c;
}

// A plain left-to-right grey ramp, one cycle across, as the source of Keyer and Color Lab and as a modulator.
void publishGreyRamp(int bus) {
	RampsParams p;
	p.axisA = 0;
	p.freqA = 1.f;
	p.phaseA = 0.f;
	p.waveA = 0.f;
	p.mix = 0;
	videoBusPublishRamps(bus, p);
}

// ---- Section 1: nothing patched, the pictures as they were. ----
void testGoldens() {
	std::printf("golden: the four modules with nothing patched\n");
	for (int precision = 0; precision < 2; ++precision) {
		const char* mode = precision ? " (full precision)" : "";
		clearBuses();
		VideoGraphRenderer r;
		r.setFullPrecision(precision != 0);
		char name[96];
		auto run = [&](const char* label, int bus, uint64_t bytesHash, uint64_t floatHash) {
			const Frame f = r.renderBus(bus, kW, kH);
			check(!f.empty() && !r.hasCycle(), "the scene renders");
			std::snprintf(name, sizeof(name), "%s%s", label, mode);
			golden(name, f, precision ? floatHash : bytesHash);
		};

		videoBusPublish(kOsc, barsParams(true));
		run("oscillator bars on bars", kOsc, 7482663407682156681ull, 2504400580180932807ull);
		videoBusPublish(kOsc, barsParams(false));
		run("oscillator bars on rows", kOsc, 7881913668908791247ull, 17978759130444669420ull);
		videoBusPublish(kOsc, plainOscParams());
		run("oscillator plain pattern", kOsc, 5230586983350614617ull, 4511800935023455799ull);

		videoBusPublishRamps(kRamp, rampParams());
		run("ramps", kRamp, 9232605243366979170ull, 9232605243366979170ull);

		publishGreyRamp(5);
		videoBusPublishKeyer(kKeyer, 5, keyerParams());
		run("keyer", kKeyer, 966668211276797219ull, 2062436392134914339ull);
		videoBusPublishColorLab(kColorLab, 5, colorLabParams());
		run("color lab", kColorLab, 10365434028700351363ull, 12204050035601780835ull);
	}
	clearBuses();
}

// ---- Helpers for the checks below ----
double lumaOf(const Frame& f, size_t px) {
	const size_t i = px * 4u;
	return (f[i] * 0.2126 + f[i + 1] * 0.7152 + f[i + 2] * 0.0722) / 255.0;
}

// The brightness of a Bars and Rows picture drawn at full saturation: its brightest channel (value in HSV).
double valueAt(const Frame& f, int x, int y) {
	const size_t i = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
	return std::max({f[i], f[i + 1], f[i + 2]});
}

// Positions, in pixel indices, where the line goes up through mid level.
std::vector<double> risingCrossings(const Frame& f, int y) {
	std::vector<double> out;
	for (int x = 0; x + 1 < kW; ++x) {
		const double a = valueAt(f, x, y);
		const double b = valueAt(f, x + 1, y);
		if (a < 127.5 && b >= 127.5) {
			out.push_back(x + (127.5 - a) / (b - a));
		}
	}
	return out;
}

int countIn(const std::vector<double>& v, double lo, double hi) {
	int n = 0;
	for (double x : v) {
		if (x >= lo && x < hi) ++n;
	}
	return n;
}

// Oscillator A on bars, sine, A alone, locked: the picture a FM IN test measures.
VideoParams fmBars(double cycles, int fmBus, float amount) {
	VideoParams p = barsParams(true);
	p.cyclesA = static_cast<float>(cycles);
	p.mix = 0;
	p.fmBusId = fmBus;
	p.fmAmount = amount;
	return p;
}

// What the engine promises for a line of BARS: A's phase is the running sum of its rate, A's cycles per pixel times
// 1 + amount x 1.5 x (luma - 0.5). lumaOfPixel(x) is the modulating picture's brightness at pixel x of the line.
template <class Luma>
std::vector<double> predictedPhase(double cycles, double amount, Luma lumaOfPixel) {
	std::vector<double> phase(static_cast<size_t>(kW));
	double sum = 0.0;
	for (int x = 0; x < kW; ++x) {
		sum += cycles / kW * amount * kPairFmDepth * (lumaOfPixel(x) - 0.5);
		phase[static_cast<size_t>(x)] = cycles * (x + 0.5) / kW + sum;
	}
	return phase;
}

// Where a rising sine crosses mid level, in pixel indices: the phase passes n + 0.25.
std::vector<double> predictedCrossings(const std::vector<double>& phase) {
	std::vector<double> out;
	for (size_t x = 0; x + 1 < phase.size(); ++x) {
		const double a = phase[x] - 0.25;
		const double b = phase[x + 1] - 0.25;
		if (std::floor(b) > std::floor(a)) {
			const double target = std::floor(b);
			out.push_back(static_cast<double>(x) + (target - a) / (b - a));
		}
	}
	return out;
}

// Largest distance from a measured crossing to the predicted one with the same index, and the RMS.
struct CrossingError {
	double maxPx = 0.0;
	double rmsPx = 0.0;
	size_t matched = 0;
	bool sameCount = false;
};
CrossingError compareCrossings(const std::vector<double>& measured, const std::vector<double>& predicted) {
	CrossingError e;
	e.sameCount = measured.size() == predicted.size();
	const size_t n = std::min(measured.size(), predicted.size());
	double sq = 0.0;
	for (size_t i = 0; i < n; ++i) {
		const double d = std::abs(measured[i] - predicted[i]);
		e.maxPx = std::max(e.maxPx, d);
		sq += d * d;
	}
	e.matched = n;
	e.rmsPx = n ? std::sqrt(sq / static_cast<double>(n)) : 0.0;
	return e;
}

Frame renderOnce(int bus, bool precision = false) {
	VideoGraphRenderer r;
	r.setFullPrecision(precision);
	return r.renderBus(bus, kW, kH);
}

// ---- Section 2: no picture, or no amount: byte for byte today's picture ----
void testUnpatchedIsUntouched() {
	std::printf("unpatched: a cable that does nothing leaves the picture as it was\n");
	for (int precision = 0; precision < 2; ++precision) {
		clearBuses();
		publishGreyRamp(5);  // an active modulation bus
		VideoParams bars = barsParams(true);
		VideoParams rows = barsParams(false);
		VideoParams plain = plainOscParams();
		videoBusPublish(kOsc, bars);
		const uint64_t barsHash = hashFrame(renderOnce(kOsc, precision != 0));
		videoBusPublish(kOsc, rows);
		const uint64_t rowsHash = hashFrame(renderOnce(kOsc, precision != 0));
		videoBusPublish(kOsc, plain);
		const uint64_t plainHash = hashFrame(renderOnce(kOsc, precision != 0));
		RampsParams ramps = rampParams();
		videoBusPublishRamps(kRamp, ramps);
		const uint64_t rampsHash = hashFrame(renderOnce(kRamp, precision != 0));
		publishGreyRamp(7);
		videoBusPublishKeyer(kKeyer, 7, keyerParams());
		const uint64_t keyerHash = hashFrame(renderOnce(kKeyer, precision != 0));
		videoBusPublishColorLab(kColorLab, 7, colorLabParams());
		const uint64_t labHash = hashFrame(renderOnce(kColorLab, precision != 0));

		// Amount zero, cable patched to a live picture.
		bars.fmBusId = 5; bars.fmAmount = 0.f;
		rows.fmBusId = 5; rows.fmAmount = 0.f;
		plain.fmBusId = 5; plain.fmAmount = 0.f;
		videoBusPublish(kOsc, bars);
		check(hashFrame(renderOnce(kOsc, precision != 0)) == barsHash, "oscillator bars, FM amount 0 with a cable: identical");
		videoBusPublish(kOsc, rows);
		check(hashFrame(renderOnce(kOsc, precision != 0)) == rowsHash, "oscillator rows, FM amount 0 with a cable: identical");
		videoBusPublish(kOsc, plain);
		check(hashFrame(renderOnce(kOsc, precision != 0)) == plainHash, "oscillator plain pattern, FM amount 0 with a cable: identical");
		ramps.modBusId = 5; ramps.modAmount = 0.f;
		videoBusPublishRamps(kRamp, ramps);
		check(hashFrame(renderOnce(kRamp, precision != 0)) == rampsHash, "ramps, MOD amount 0 with a cable: identical");
		KeyerParams key = keyerParams();
		key.modBusId = 5; key.modAmount = 0.f;
		videoBusPublishKeyer(kKeyer, 7, key);
		check(hashFrame(renderOnce(kKeyer, precision != 0)) == keyerHash, "keyer, MOD amount 0 with a cable: identical");
		ColorLabParams lab = colorLabParams();
		lab.modBusId = 5; lab.modAmount = 0.f;
		videoBusPublishColorLab(kColorLab, 7, lab);
		check(hashFrame(renderOnce(kColorLab, precision != 0)) == labHash, "color lab, MOD amount 0 with a cable: identical");

		// An amount, but the cable's bus has no picture: no modulation.
		bars.fmBusId = 40; bars.fmAmount = 1.f;
		rows.fmBusId = 40; rows.fmAmount = 1.f;
		plain.fmBusId = 40; plain.fmAmount = 1.f;
		videoBusPublish(kOsc, bars);
		check(hashFrame(renderOnce(kOsc, precision != 0)) == barsHash, "oscillator bars, FM from an empty bus: identical");
		videoBusPublish(kOsc, rows);
		check(hashFrame(renderOnce(kOsc, precision != 0)) == rowsHash, "oscillator rows, FM from an empty bus: identical");
		videoBusPublish(kOsc, plain);
		check(hashFrame(renderOnce(kOsc, precision != 0)) == plainHash, "oscillator plain pattern, FM from an empty bus: identical");
		ramps.modBusId = 40; ramps.modAmount = 1.f;
		videoBusPublishRamps(kRamp, ramps);
		check(hashFrame(renderOnce(kRamp, precision != 0)) == rampsHash, "ramps, MOD from an empty bus: identical");
		key.modBusId = 40; key.modAmount = 1.f;
		videoBusPublishKeyer(kKeyer, 7, key);
		check(hashFrame(renderOnce(kKeyer, precision != 0)) == keyerHash, "keyer, MOD from an empty bus: identical");
		lab.modBusId = 40; lab.modAmount = 1.f;
		videoBusPublishColorLab(kColorLab, 7, lab);
		check(hashFrame(renderOnce(kColorLab, precision != 0)) == labHash, "color lab, MOD from an empty bus: identical");

		// An amount, and the module's own bus: no modulation.
		bars.fmBusId = kOsc;
		videoBusPublish(kOsc, bars);
		check(hashFrame(renderOnce(kOsc, precision != 0)) == barsHash, "oscillator bars, FM from its own bus: identical");
		ramps.modBusId = kRamp;
		videoBusPublishRamps(kRamp, ramps);
		check(hashFrame(renderOnce(kRamp, precision != 0)) == rampsHash, "ramps, MOD from its own bus: identical");
		key.modBusId = kKeyer;
		videoBusPublishKeyer(kKeyer, 7, key);
		check(hashFrame(renderOnce(kKeyer, precision != 0)) == keyerHash, "keyer, MOD from its own bus: identical");
		lab.modBusId = kColorLab;
		videoBusPublishColorLab(kColorLab, 7, lab);
		check(hashFrame(renderOnce(kColorLab, precision != 0)) == labHash, "color lab, MOD from its own bus: identical");

		// With an amount and a live picture every one of them changes.
		bars.fmBusId = 5; bars.fmAmount = 0.5f;
		rows.fmBusId = 5; rows.fmAmount = 0.5f;
		plain.fmBusId = 5; plain.fmAmount = 0.5f;
		videoBusPublish(kOsc, bars);
		check(hashFrame(renderOnce(kOsc, precision != 0)) != barsHash, "oscillator bars change with a picture and an amount");
		videoBusPublish(kOsc, rows);
		check(hashFrame(renderOnce(kOsc, precision != 0)) != rowsHash, "oscillator rows change with a picture and an amount");
		videoBusPublish(kOsc, plain);
		check(hashFrame(renderOnce(kOsc, precision != 0)) != plainHash, "oscillator plain pattern changes with a picture and an amount");
		ramps.modBusId = 5; ramps.modAmount = 0.5f;
		videoBusPublishRamps(kRamp, ramps);
		check(hashFrame(renderOnce(kRamp, precision != 0)) != rampsHash, "ramps change with a picture and an amount");
		key.modBusId = 5; key.modAmount = 0.5f;
		videoBusPublishKeyer(kKeyer, 7, key);
		check(hashFrame(renderOnce(kKeyer, precision != 0)) != keyerHash, "keyer changes with a picture and an amount");
		// (Color Lab's hue shift does nothing to a grey picture; its own test below uses a coloured one.)
	}
	clearBuses();
}

// ---- Section 3: Video Oscillator FM ----
void testFmEngineFlatGrey() {
	std::printf("video oscillator FM: a picture of exactly mid grey is no modulation\n");
	std::vector<float> half(static_cast<size_t>(kW) * kH, 0.5f);
	for (int pattern = 0; pattern < 3; ++pattern) {
		VideoParams p = pattern == 0 ? barsParams(true) : pattern == 1 ? barsParams(false) : plainOscParams();
		PreviewVideoEngine plainEngine;
		const Frame none = plainEngine.render(kW, kH, p);
		p.fmAmount = 1.f;
		PreviewVideoEngine fmEngine;
		const Frame withFm = fmEngine.render(kW, kH, p, false, half.data());
		check(none == withFm, pattern == 0 ? "bars: luma 0.5 everywhere draws the unmodulated picture" : pattern == 1 ? "rows: luma 0.5 everywhere draws the unmodulated picture" : "plain pattern: luma 0.5 everywhere draws the unmodulated picture");
		// Float output too.
		PreviewVideoEngine plainF, fmF;
		p.fmAmount = 0.f;
		plainF.render(kW, kH, p, true);
		const std::vector<float> noneF = plainF.outputF();
		p.fmAmount = 1.f;
		fmF.render(kW, kH, p, true, half.data());
		check(noneF == fmF.outputF(), "and in a float picture");
	}
}

void testFmFlatGreyDithered() {
	std::printf("video oscillator FM: a flat mid grey in the graph\n");
	clearBuses();
	// A picture that averages exactly 127.5: alternate pixels 127 and 128.
	const int frameId = videoExternalFrameAllocate();
	std::vector<unsigned char> px(static_cast<size_t>(kW) * kH * 4u);
	for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i) {
		const unsigned char v = static_cast<unsigned char>((i % 2) ? 128 : 127);
		px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
		px[i * 4 + 3] = 255;
	}
	videoExternalFramePublish(frameId, px.data(), kW, kH, 0.0);
	videoBusPublishExternalFrame(5, frameId);
	videoBusPublish(kOsc, fmBars(40.0, 0, 0.f));
	const Frame none = renderOnce(kOsc);
	videoBusPublish(kOsc, fmBars(40.0, 5, 1.f));
	const Frame grey = renderOnce(kOsc);
	int worst = 0;
	for (size_t i = 0; i < none.size(); ++i) {
		worst = std::max(worst, std::abs(static_cast<int>(none[i]) - static_cast<int>(grey[i])));
	}
	std::printf("  largest difference with a 127/128 grey on FM IN: %d levels\n", worst);
	check(worst <= 3, "a flat mid-grey picture on FM IN matches no modulation to within 3 levels");
	videoExternalFrameRelease(frameId);
	clearBuses();
}

void testFmChirp() {
	std::printf("video oscillator FM: a left-to-right ramp makes a chirp\n");
	clearBuses();
	publishGreyRamp(5);
	const Frame ramp = renderOnce(5);
	auto rampLuma = [&](int x) { return lumaOf(ramp, static_cast<size_t>(90) * kW + static_cast<size_t>(x)); };
	for (int sign = 0; sign < 2; ++sign) {
		const double amount = sign == 0 ? 1.0 : -1.0;
		const double cycles = 40.0;
		videoBusPublish(kOsc, fmBars(cycles, 5, static_cast<float>(amount)));
		const Frame f = renderOnce(kOsc);
		const std::vector<double> measured = risingCrossings(f, 90);
		const std::vector<double> predicted = predictedCrossings(predictedPhase(cycles, amount, rampLuma));
		const CrossingError err = compareCrossings(measured, predicted);
		std::printf("  amount %+.0f: %zu crossings measured, %zu predicted, largest error %.3f px, rms %.3f px\n", amount, measured.size(), predicted.size(), err.maxPx, err.rmsPx);
		check(err.sameCount, "the number of bars matches the formula");
		check(err.maxPx < 0.35, "every bar sits where the running-sum formula puts it, to 0.35 px");

		// Bars in the left and right thirds. Rate runs 1 + amount x 1.5 x (x/W - 0.5): the cycles in a third are
		// cycles x (1/3 -/+ amount x 1.5 / 9), so right : left is 3 at amount +1 and 1/3 at amount -1.
		const double left = countIn(measured, 0, kW / 3.0);
		const double right = countIn(measured, 2.0 * kW / 3.0, kW);
		const double expectLeft = cycles * (1.0 / 3.0 - amount * kPairFmDepth / 9.0);
		const double expectRight = cycles * (1.0 / 3.0 + amount * kPairFmDepth / 9.0);
		std::printf("  bars in the left third %.0f (formula %.2f), right third %.0f (formula %.2f), ratio %.2f (formula %.2f)\n", left, expectLeft, right, expectRight, right / left, expectRight / expectLeft);
		check(std::abs(left - expectLeft) <= 1.0 && std::abs(right - expectRight) <= 1.0, "bar counts in the left and right thirds match the formula within one bar");
		check(std::abs(right / left - expectRight / expectLeft) < 0.45 * (expectRight / expectLeft > 1.0 ? 1.0 : 0.25), "right to left ratio matches the formula");
	}
	// With no modulation the spacing is even.
	videoBusPublish(kOsc, fmBars(40.0, 0, 0.f));
	const std::vector<double> flat = risingCrossings(renderOnce(kOsc), 90);
	check(countIn(flat, 0, kW / 3.0) >= 13 && countIn(flat, 0, kW / 3.0) <= 14, "unmodulated A has even spacing: 13 or 14 bars in the left third");
	clearBuses();
}

void testFmRows() {
	std::printf("video oscillator FM: rows take the sum down each field\n");
	clearBuses();
	// A vertical grey ramp (dark at the top, bright at the bottom) on FM IN, A on ROWS: the rows bunch towards the bottom.
	RampsParams vertical;
	vertical.axisA = 1;
	vertical.freqA = 1.f;
	vertical.mix = 0;
	videoBusPublishRamps(5, vertical);
	VideoParams rows = barsParams(false);
	rows.mix = 0;
	rows.cyclesA = 20.f;
	rows.fmBusId = 5;
	rows.fmAmount = 1.f;
	videoBusPublish(kOsc, rows);
	const Frame bent = renderOnce(kOsc);
	rows.fmAmount = 0.f;
	videoBusPublish(kOsc, rows);
	const Frame straight = renderOnce(kOsc);
	check(bent != straight, "a picture on FM IN bends A's rows");
	auto crossingsDown = [&](const Frame& f, int x) {
		std::vector<double> out;
		for (int y = 0; y + 1 < kH; ++y) {
			const double a = valueAt(f, x, y);
			const double b = valueAt(f, x, y + 1);
			if (a < 127.5 && b >= 127.5) out.push_back(y + (127.5 - a) / (b - a));
		}
		return out;
	};
	const std::vector<double> down = crossingsDown(bent, 160);
	const double top = countIn(down, 0, kH / 3.0);
	const double bottom = countIn(down, 2.0 * kH / 3.0, kH);
	std::printf("  rows in the top third %.0f, bottom third %.0f\n", top, bottom);
	check(bottom > 2.0 * top, "bright at the bottom bunches the rows there: more than twice as many as at the top");
	const std::vector<double> evenDown = crossingsDown(straight, 160);
	check(std::abs(countIn(evenDown, 0, kH / 3.0) - countIn(evenDown, 2.0 * kH / 3.0, kH)) <= 1, "unmodulated rows are evenly spaced");
	clearBuses();
}

void testFmFollowsAnOscillator() {
	std::printf("video oscillator FM: a second oscillator's picture bends the bars with its own shape\n");
	clearBuses();
	// Modulator: bars across, 4 cycles, sine, A alone, turned grey by a Color Lab with no saturation.
	VideoParams mod = barsParams(true);
	mod.cyclesA = 4.f;
	mod.mix = 0;
	videoBusPublish(6, mod);
	ColorLabParams grey;
	grey.saturation = 0.f;
	videoBusPublishColorLab(7, 6, grey);
	const double cycles = 32.0;  // 8 bars in each quarter of the picture
	videoBusPublish(kOsc, fmBars(cycles, 7, 0.f));
	const Frame straight = renderOnce(kOsc);
	videoBusPublish(kOsc, fmBars(cycles, 7, 1.f));
	const Frame bent = renderOnce(kOsc);
	long diff = 0;
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			diff += std::abs(static_cast<int>(valueAt(bent, x, y)) - static_cast<int>(valueAt(straight, x, y)));
		}
	}
	const double meanDiff = static_cast<double>(diff) / (kW * kH);
	std::printf("  mean difference from the unmodulated picture: %.1f levels\n", meanDiff);
	check(meanDiff > 10.0, "the bars are visibly bent by the second oscillator");
	// The modulator repeats every quarter of the picture (80 px) and has mean brightness 0.5, so the bend repeats too:
	// A has 8 bars in each 80 px, and bar n + 8 sits 80 px from bar n. (A byte picture is a little under its true
	// brightness, so the bars drift by a fraction of a pixel in each period; the check allows 1 px.)
	const std::vector<double> a = risingCrossings(straight, 90);
	const std::vector<double> b = risingCrossings(bent, 90);
	double worstRepeat = 0.0;
	for (size_t i = 0; i + 8 < b.size(); ++i) {
		worstRepeat = std::max(worstRepeat, std::abs(b[i + 8] - b[i] - 80.0));
	}
	std::printf("  bar n + 8 is 80 px from bar n, to within %.2f px (the unbent bars: 80 px exactly)\n", worstRepeat);
	check(b.size() > 24 && worstRepeat < 1.0, "the bent pattern repeats with the modulator's period");
	// The bars follow the modulator's shape, not just any wobble: a bar's displacement is largest where the modulator's
	// brightness runs furthest from mid grey for longest, and is zero where the running sum comes back to zero.
	double maxShift = 0.0;
	if (a.size() == b.size()) {
		for (size_t i = 0; i < a.size(); ++i) maxShift = std::max(maxShift, std::abs(a[i] - b[i]));
	}
	std::printf("  largest bar displacement: %.2f px (bars %zu and %zu)\n", maxShift, a.size(), b.size());
	check(a.size() == b.size() && maxShift > 1.0, "the bars move by more than a pixel");
	clearBuses();
}

// ---- Section 4: loops, delays, shared taps, depth ----
FrameDelayParams delayParams(int slot, int counter, int frames) {
	FrameDelayParams p;
	p.ringSlotId = slot;
	p.frameCounter = counter;
	p.frames = frames;
	return p;
}

void testLoops() {
	std::printf("loops: two oscillators bending each other\n");
	clearBuses();
	VideoParams a = barsParams(true);
	a.mix = 0;
	VideoParams b = barsParams(false);
	b.mix = 0;
	a.fmBusId = 6;
	a.fmAmount = 0.8f;
	b.fmBusId = kOsc;
	b.fmAmount = 0.8f;
	videoBusPublish(kOsc, a);
	videoBusPublish(6, b);
	VideoGraphRenderer r;
	r.renderBus(kOsc, kW, kH);
	check(r.hasCycle(), "oscillator 1 into 2's FM IN and 2 into 1's FM IN is a cycle");
	VideoGraphRenderer other;
	other.renderBus(6, kW, kH);
	check(other.hasCycle(), "a cycle from the other side too");
	// Ramps, Keyer and Color Lab report loops the same way.
	clearBuses();
	RampsParams ramps = rampParams();
	ramps.modBusId = 7;
	ramps.modAmount = 1.f;
	videoBusPublishRamps(kRamp, ramps);
	ColorLabParams lab = colorLabParams();
	videoBusPublishColorLab(7, kRamp, lab);
	VideoGraphRenderer rr;
	rr.renderBus(kRamp, kW, kH);
	check(rr.hasCycle(), "a Ramps MOD IN loop is a cycle");
	clearBuses();
	publishGreyRamp(5);
	KeyerParams key = keyerParams();
	key.modBusId = 8;
	key.modAmount = 1.f;
	videoBusPublishKeyer(kKeyer, 5, key);
	videoBusPublishColorLab(8, kKeyer, lab);
	VideoGraphRenderer kr;
	kr.renderBus(kKeyer, kW, kH);
	check(kr.hasCycle(), "a Keyer MOD IN loop is a cycle");
	clearBuses();

	// The same loop with a Frame Delay in one leg renders and evolves.
	const int slot = frameRingAllocateSlot();
	const int kDelay = 8;
	a.fmBusId = 6;
	b.fmBusId = kDelay;
	videoBusPublish(kOsc, a);
	videoBusPublish(6, b);
	VideoGraphRenderer display;
	bool noCycle = true, evolves = true;
	uint64_t last = 0;
	for (int n = 1; n <= 6; ++n) {
		videoBusPublishFrameDelay(kDelay, kOsc, delayParams(slot, n, 1));
		const Frame shown = display.renderBus(kOsc, kW, kH);
		noCycle = noCycle && !display.hasCycle() && !shown.empty();
		const uint64_t h = hashFrame(shown);
		if (n >= 2) evolves = evolves && h != last;
		last = h;
	}
	check(noCycle, "with a Frame Delay in one leg the loop renders with no cycle");
	check(evolves, "and the picture changes from tick to tick");
	frameRingReleaseSlot(slot);
	clearBuses();
}

void testSharedTap() {
	std::printf("shared taps: a modulation bus that feeds something else is drawn once\n");
	clearBuses();
	publishGreyRamp(5);
	videoBusPublish(kOsc, fmBars(20.0, 5, 0.7f));
	RampsParams ramps = rampParams();
	ramps.modBusId = 5;
	ramps.modAmount = 0.5f;
	videoBusPublishRamps(kRamp, ramps);
	// A mixer whose A is the oscillator and B is the Ramps; both of them modulated by the same picture, which the
	// mixer's matte also reads.
	BusMixerParams m;
	m.busB = kRamp;
	m.matteBus = 5;
	m.matteAmount = 1.f;
	m.mode = 6;
	videoBusPublishBusMixer(9, kOsc, m);
	VideoGraphRenderer r;
	const Frame f = r.renderBus(9, kW, kH);
	check(!f.empty() && !r.hasCycle(), "the graph renders");
	check(r.busRenderCount(5) == 1, "the modulation bus is drawn once for the oscillator, the Ramps and the matte together");
	check(r.busRenderCount(kOsc) == 1 && r.busRenderCount(kRamp) == 1, "and each module once");
	// Through the render service: two roots in one pass.
	RenderService svc;
	svc.frame(9, kW, kH);
	svc.frame(kOsc, kW, kH);
	check(svc.busRenderCount(5, kW, kH) == 1, "the render service draws the modulation bus once for two roots in a pass");
	clearBuses();
}

void testDepth() {
	std::printf("depth: the modulation bus counts as a level\n");
	for (int chain = 14; chain <= 16; ++chain) {
		clearBuses();
		publishGreyRamp(10);
		int source = 10;
		for (int i = 0; i < chain; ++i) {
			videoBusPublishColorLab(11 + i, source, colorLabParams());
			source = 11 + i;
		}
		videoBusPublish(kOsc, fmBars(20.0, source, 0.5f));
		VideoGraphRenderer r;
		const Frame f = r.renderBus(kOsc, kW, kH);
		// The oscillator is level 0, the chain levels 1 to chain, the ramp level chain + 1; 16 is the last level allowed.
		const bool tooDeep = chain + 1 > 16;
		if (tooDeep) {
			check(f.empty() && r.hasCycle(), "a chain whose ramp would be level 17 is refused");
		}
		else {
			check(!f.empty() && !r.hasCycle(), "a chain whose ramp is level 15 or 16 renders");
		}
	}
	clearBuses();
}

// ---- Section 5: Ramps, Keyer, Color Lab: the measured offset ----
void testRampsModOffset() {
	std::printf("ramps: MOD IN offsets layer A's phase by amount x 2 x (luma - 0.5) cycles\n");
	clearBuses();
	// Modulator: a left-to-right grey ramp. Layer A: a horizontal ramp, one cycle, wave 0, A alone.
	publishGreyRamp(5);
	const Frame mod = renderOnce(5);
	for (int sign = 0; sign < 2; ++sign) {
		const float amount = sign == 0 ? 0.25f : -0.4f;
		RampsParams p;
		p.axisA = 0;
		p.freqA = 1.f;
		p.phaseA = 0.1f;
		p.waveA = 0.f;
		p.mix = 0;
		p.modBusId = 5;
		p.modAmount = amount;
		videoBusPublishRamps(kRamp, p);
		const Frame f = renderOnce(kRamp);
		int worst = 0;
		int wraps = 0;
		int compared = 0;
		for (int y = 0; y < kH; y += 11) {
			for (int x = 0; x < kW; ++x) {
				const double l = lumaOf(mod, static_cast<size_t>(y) * kW + static_cast<size_t>(x));
				double pos = static_cast<double>(x) / kW * 1.0 + 0.1 + amount * 2.0 * (l - 0.5);
				pos -= std::floor(pos);
				const int expected = static_cast<int>(std::lround(255.0 * pos));
				const int got = f[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u];
				const int d = std::abs(expected - got);
				if (d > 200) {
					++wraps;  // a pixel on the wrap edge: rounding can land it either side
				}
				else {
					worst = std::max(worst, d);
					++compared;
				}
			}
		}
		std::printf("  amount %+.2f: %d pixels compared, largest difference from the formula %d level(s), %d wrap-edge pixels set aside\n", amount, compared, worst, wraps);
		check(worst <= 1, "layer A's grey is the ramp shifted by amount x 2 x (luma - 0.5) cycles, to one level");
		check(wraps <= 4 * (kH / 11 + 1), "only the pixels on the wrap edge are set aside");
	}
	clearBuses();
}

void testKeyerModOffset() {
	std::printf("keyer: MOD IN moves the threshold by amount x 2 x (luma - 0.5)\n");
	clearBuses();
	publishGreyRamp(5);  // the keyed picture: left to right
	RampsParams vertical;
	vertical.axisA = 1;
	vertical.freqA = 1.f;
	vertical.mix = 0;
	videoBusPublishRamps(6, vertical);  // the modulator: top to bottom
	const Frame source = renderOnce(5);
	const Frame mod = renderOnce(6);
	KeyerParams k;
	k.threshold = 0.5f;
	k.softness = 0.1f;
	k.gain = 1.f;
	k.mode = 0;
	k.modBusId = 6;
	k.modAmount = 0.3f;
	videoBusPublishKeyer(kKeyer, 5, k);
	const Frame f = renderOnce(kKeyer);
	int worst = 0;
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const size_t px = static_cast<size_t>(y) * kW + static_cast<size_t>(x);
			const double l = std::clamp((source[px * 4] * 0.2126 + source[px * 4 + 1] * 0.7152 + source[px * 4 + 2] * 0.0722) / 255.0, 0.0, 1.0);
			const double m = lumaOf(mod, px);
			const double thr = std::clamp(0.5 + 0.3 * 2.0 * (m - 0.5), 0.0, 1.0);
			const double matte = std::clamp((l - thr + 0.05) / 0.1, 0.0, 1.0);
			worst = std::max(worst, std::abs(static_cast<int>(matte * 255.0) - static_cast<int>(f[px * 4])));
		}
	}
	std::printf("  largest difference from the formula: %d level(s)\n", worst);
	check(worst <= 2, "the matte is the key with a threshold of 0.5 + 0.3 x 2 x (luma - 0.5) at every pixel, to two levels");
	// The key edge (matte 0.5) runs along x = threshold x width: it slants from left at the top to right at the bottom.
	auto edgeX = [&](int y) {
		for (int x = 0; x < kW; ++x) {
			if (f[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u] >= 128) return x;
		}
		return kW;
	};
	const int top = edgeX(2);
	const int bottom = edgeX(kH - 3);
	std::printf("  the key opens at x = %d near the top and x = %d near the bottom (expected about %.0f and %.0f)\n", top, bottom, 0.5 * kW + 0.3 * 2.0 * (2.5 / kH - 0.5) * kW, 0.5 * kW + 0.3 * 2.0 * ((kH - 2.5) / kH - 0.5) * kW);
	check(top < bottom - 40, "the key opens further left at the top than at the bottom");
	clearBuses();
}

void testColorLabModOffset() {
	std::printf("color lab: MOD IN shifts the hue by amount x (luma - 0.5) turns\n");
	clearBuses();
	const int frameId = videoExternalFrameAllocate();
	std::vector<unsigned char> red(static_cast<size_t>(kW) * kH * 4u);
	for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i) {
		red[i * 4] = 255;
		red[i * 4 + 1] = 0;
		red[i * 4 + 2] = 0;
		red[i * 4 + 3] = 255;
	}
	videoExternalFramePublish(frameId, red.data(), kW, kH, 0.0);
	videoBusPublishExternalFrame(5, frameId);  // a flat red picture
	publishGreyRamp(6);                         // the modulator: left to right
	const Frame mod = renderOnce(6);
	ColorLabParams c;
	c.hue = 0.f;
	c.modBusId = 6;
	c.modAmount = 0.6f;
	videoBusPublishColorLab(kColorLab, 5, c);
	const Frame f = renderOnce(kColorLab);
	auto hueOf = [&](size_t px) {
		const double r = f[px * 4] / 255.0, g = f[px * 4 + 1] / 255.0, b = f[px * 4 + 2] / 255.0;
		const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
		const double d = mx - mn;
		if (d <= 0.0) return 0.0;
		double h = mx == r ? std::fmod((g - b) / d + 6.0, 6.0) : mx == g ? (b - r) / d + 2.0 : (r - g) / d + 4.0;
		h /= 6.0;
		return h - std::floor(h);
	};
	double worst = 0.0;
	for (int y = 0; y < kH; y += 13) {
		for (int x = 0; x < kW; ++x) {
			const size_t px = static_cast<size_t>(y) * kW + static_cast<size_t>(x);
			double expected = 0.6 * (lumaOf(mod, px) - 0.5);
			expected -= std::floor(expected);
			double d = std::abs(hueOf(px) - expected);
			d = std::min(d, 1.0 - d);
			worst = std::max(worst, d);
		}
	}
	std::printf("  largest hue difference from the formula: %.4f turns\n", worst);
	check(worst < 0.01, "each pixel's hue is amount x (luma - 0.5) turns, to a hundredth of a turn");
	videoExternalFrameRelease(frameId);
	clearBuses();
}

// ---- Section 6: full precision ----
void testPrecision() {
	std::printf("precision: a float ramp on FM IN gives a smoother bend than a byte ramp\n");
	clearBuses();
	// A dim ramp, 0 to 12.5% brightness, so that bytes have only about 32 steps across the picture and floats have none.
	RampsParams shallow;
	shallow.axisA = 0;
	shallow.freqA = 0.25f;
	shallow.waveA = 0.f;
	shallow.mix = 0;
	videoBusPublishRamps(5, shallow);
	ColorLabParams dim;
	dim.brightness = 0.5f;
	videoBusPublishColorLab(12, 5, dim);
	const double cycles = 60.0;
	videoBusPublish(kOsc, fmBars(cycles, 12, 1.f));
	// The exact bend: the ramp is x / W, times 0.25 and then 0.5.
	auto exactLuma = [&](int x) { return 0.125 * static_cast<double>(x) / kW; };
	const std::vector<double> predicted = predictedCrossings(predictedPhase(cycles, 1.0, exactLuma));
	const Frame bytes = renderOnce(kOsc, false);
	const Frame floats = renderOnce(kOsc, true);
	const CrossingError eb = compareCrossings(risingCrossings(bytes, 90), predicted);
	const CrossingError ef = compareCrossings(risingCrossings(floats, 90), predicted);
	std::printf("  distance of the bars from the exact bend, rms: byte ramp %.3f px, float ramp %.3f px (largest %.3f and %.3f px)\n", eb.rmsPx, ef.rmsPx, eb.maxPx, ef.maxPx);
	check(ef.sameCount, "the float ramp gives the same number of bars as the formula");
	check(ef.rmsPx < eb.rmsPx, "the float ramp's bars sit closer to the exact bend than the byte ramp's");
	// The same, with nothing else rounded: the engine draws a float picture from the same ramp, once with the ramp as a
	// float and once with every value rounded to a level of 1/255 (what a byte picture hands on).
	std::vector<float> smooth(static_cast<size_t>(kW) * kH), stepped(static_cast<size_t>(kW) * kH);
	for (int y = 0; y < kH; ++y) {
		for (int x = 0; x < kW; ++x) {
			const float v = 0.125f * static_cast<float>(x) / kW;
			smooth[static_cast<size_t>(y) * kW + static_cast<size_t>(x)] = v;
			stepped[static_cast<size_t>(y) * kW + static_cast<size_t>(x)] = std::floor(v * 255.f + 0.5f) / 255.f;
		}
	}
	auto crossingsF = [&](const std::vector<float>& f) {
		std::vector<double> out;
		for (int x = 0; x + 1 < kW; ++x) {
			auto value = [&](int xx) {
				const size_t i = (static_cast<size_t>(90) * kW + static_cast<size_t>(xx)) * 4u;
				return static_cast<double>(std::max({f[i], f[i + 1], f[i + 2]}));
			};
			const double a = value(x), b = value(x + 1);
			if (a < 127.5 && b >= 127.5) out.push_back(x + (127.5 - a) / (b - a));
		}
		return out;
	};
	PreviewVideoEngine engineSmooth, engineStepped;
	VideoParams withFm = fmBars(cycles, 0, 1.f);
	withFm.fmAmount = 1.f;
	engineSmooth.render(kW, kH, withFm, true, smooth.data());
	engineStepped.render(kW, kH, withFm, true, stepped.data());
	const CrossingError es = compareCrossings(crossingsF(engineSmooth.outputF()), predicted);
	const CrossingError et = compareCrossings(crossingsF(engineStepped.outputF()), predicted);
	std::printf("  on a float picture, distance of the bars from the exact bend: unrounded ramp rms %.4f px (largest %.4f), ramp cut to 1/255 steps rms %.4f px (largest %.4f)\n", es.rmsPx, es.maxPx, et.rmsPx, et.maxPx);
	check(es.sameCount && et.sameCount, "the same number of bars in both");
	check(es.rmsPx < 0.2 * et.rmsPx, "an unrounded ramp is at least five times closer to the exact bend than the same ramp cut to 1/255 steps");
	check(ef.rmsPx < 0.05, "the float ramp follows the exact bend to 0.05 px rms");
	check(eb.rmsPx > 10.0 * ef.rmsPx, "and the byte ramp is at least ten times further from it");
	// Deterministic: a second renderer draws the same bytes.
	check(hashFrame(renderOnce(kOsc, true)) == hashFrame(floats), "full precision is deterministic");
	// And in the graph with Ramps, Keyer and Color Lab modulated, twice over.
	RampsParams rp = rampParams();
	rp.modBusId = 12;
	rp.modAmount = 1.f;
	videoBusPublishRamps(kRamp, rp);
	KeyerParams kp = keyerParams();
	kp.modBusId = 12;
	kp.modAmount = 1.f;
	videoBusPublishKeyer(kKeyer, 5, kp);
	ColorLabParams cl = colorLabParams();
	cl.modBusId = 12;
	cl.modAmount = 1.f;
	videoBusPublishColorLab(kColorLab, 5, cl);
	const int busesToCheck[3] = {kRamp, kKeyer, kColorLab};
	for (int bus : busesToCheck) {
		check(hashFrame(renderOnce(bus, true)) == hashFrame(renderOnce(bus, true)), "a modulated module draws the same picture twice with full precision on");
	}
	clearBuses();
}

}  // namespace

int main() {
	testGoldens();
	testUnpatchedIsUntouched();
	testFmEngineFlatGrey();
	testFmFlatGreyDithered();
	testFmChirp();
	testFmRows();
	testFmFollowsAnOscillator();
	testLoops();
	testSharedTap();
	testDepth();
	testRampsModOffset();
	testKeyerModOffset();
	testColorLabModOffset();
	testPrecision();
	std::printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
