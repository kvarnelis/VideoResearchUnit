// Bus Mixer and Wave Shaper look large byte pictures up in a table (src/video_graph_renderer.cpp,
// renderBusMixerFrame and renderWaveShaperFrame). The table must give the very bytes the pixel loop gives.
// Offline, no Rack SDK.
//
// A small picture is still drawn by the pixel loop, so small pictures give the reference: for the Bus Mixer the
// output byte for every pair of input bytes, for the Wave Shaper the output byte for every input byte. A picture
// large enough to use the table is then checked against that reference, byte by byte, for every blend mode and a
// set of mixes, and for a set of Wave Shaper settings. Every one of the 65536 pairs (256 values) is in the pictures.

#include "video_bus.hpp"
#include "video_external_frame.hpp"
#include "video_frame.hpp"
#include "video_graph_renderer.hpp"

#include <cstdio>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL: %s\n", what);
	}
}

constexpr int kBusA = 10;
constexpr int kBusB = 11;
constexpr int kBusOut = 12;

// One external picture on a bus, replaced by publish().
struct Source {
	int id = 0;
	int bus = 0;
	explicit Source(int busId) : id(videoExternalFrameAllocate()), bus(busId) {}
	~Source() {
		videoBusClear(bus);
		videoExternalFrameRelease(id);
	}
	void publish(const Bytes& rgba, int w, int h) {
		videoExternalFramePublish(id, rgba.data(), w, h, 0.0);
		videoBusPublishExternalFrame(bus, id);
	}
};

Bytes render(VideoGraphRenderer& r, int bus, int w, int h) {
	const VideoGraphRenderer::Frame& f = r.renderBus(bus, w, h);
	return Bytes(f.begin(), f.end());
}

Bytes picture(int w, int h) {
	Bytes p(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 255);
	return p;
}

// ---- Bus Mixer ----

constexpr int kStripH = 16;  // 256 x 16 = 4096 pixels: far below the table's size, so the pixel loop draws it

void testBusMixer() {
	std::printf("Bus Mixer table against the pixel loop\n");
	Source a(kBusA);
	Source b(kBusB);
	VideoGraphRenderer small;
	VideoGraphRenderer large;

	// The large pictures, 256 x 256. Every channel holds every pair once: red (y, x), green (x, y), blue (255 - y, x).
	Bytes largeA = picture(256, 256);
	Bytes largeB = picture(256, 256);
	for (int y = 0; y < 256; ++y) {
		for (int x = 0; x < 256; ++x) {
			const size_t i = (static_cast<size_t>(y) * 256u + static_cast<size_t>(x)) * 4u;
			largeA[i + 0] = static_cast<unsigned char>(y);
			largeB[i + 0] = static_cast<unsigned char>(x);
			largeA[i + 1] = static_cast<unsigned char>(x);
			largeB[i + 1] = static_cast<unsigned char>(y);
			largeA[i + 2] = static_cast<unsigned char>(255 - y);
			largeB[i + 2] = static_cast<unsigned char>(x);
		}
	}

	// An external picture drawn at its own size arrives as it is; the rest of the test leans on that.
	a.publish(largeA, 256, 256);
	check(render(large, kBusA, 256, 256) == largeA, "an external picture at its own size is not resampled");

	const float mixes[] = {0.f, 0.25f, 0.38f, 0.5f, 0.82f, 1.f};
	int combos = 0;
	size_t wrong = 0;
	size_t channelsDisagree = 0;
	size_t alphaWrong = 0;
	// Twice over: the second time round every table is rebuilt after a different one was in place.
	for (int round = 0; round < 2; ++round) {
		for (int mode = 0; mode <= 6; ++mode) {
			for (float mix : mixes) {
				BusMixerParams p;
				p.busB = kBusB;
				p.matteBus = 0;
				p.mode = mode;
				p.mix = mix;
				videoBusPublishBusMixer(kBusOut, kBusA, p);

				// Reference: 16 strips of 256 x 16, grey pictures, A = the row's value, B = the column.
				std::vector<unsigned char> ref(65536u);
				for (int s = 0; s < 256 / kStripH; ++s) {
					Bytes stripA = picture(256, kStripH);
					Bytes stripB = picture(256, kStripH);
					for (int r = 0; r < kStripH; ++r) {
						for (int x = 0; x < 256; ++x) {
							const size_t i = (static_cast<size_t>(r) * 256u + static_cast<size_t>(x)) * 4u;
							for (size_t c = 0; c < 3u; ++c) {
								stripA[i + c] = static_cast<unsigned char>(s * kStripH + r);
								stripB[i + c] = static_cast<unsigned char>(x);
							}
						}
					}
					a.publish(stripA, 256, kStripH);
					b.publish(stripB, 256, kStripH);
					const Bytes out = render(small, kBusOut, 256, kStripH);
					if (out.size() != stripA.size()) {
						check(false, "a Bus Mixer strip rendered");
						return;
					}
					for (int r = 0; r < kStripH; ++r) {
						for (int x = 0; x < 256; ++x) {
							const size_t i = (static_cast<size_t>(r) * 256u + static_cast<size_t>(x)) * 4u;
							ref[static_cast<size_t>(s * kStripH + r) * 256u + static_cast<size_t>(x)] = out[i];
							if (out[i + 1] != out[i] || out[i + 2] != out[i]) ++channelsDisagree;
							if (out[i + 3] != 255) ++alphaWrong;
						}
					}
				}

				a.publish(largeA, 256, 256);
				b.publish(largeB, 256, 256);
				const Bytes out = render(large, kBusOut, 256, 256);
				if (out.size() != largeA.size()) {
					check(false, "the large Bus Mixer picture rendered");
					return;
				}
				for (size_t i = 0; i < out.size(); i += 4u) {
					for (size_t c = 0; c < 3u; ++c) {
						if (out[i + c] != ref[static_cast<size_t>(largeA[i + c]) * 256u + largeB[i + c]]) ++wrong;
					}
					if (out[i + 3] != 255) ++alphaWrong;
				}
				++combos;
			}
		}
	}
	std::printf("  %d mode and mix settings, 65536 pairs each in three channels: %zu bytes differ\n", combos, wrong);
	check(wrong == 0, "the Bus Mixer table gives the pixel loop's bytes for every pair, mode and mix");
	check(channelsDisagree == 0, "the Bus Mixer gives a grey pair the same byte in all three channels");
	check(alphaWrong == 0, "Bus Mixer alpha is 255");
	videoBusClear(kBusOut);
}

// ---- Wave Shaper ----

void testWaveShaper() {
	std::printf("Wave Shaper table against the pixel loop\n");
	Source a(kBusA);
	VideoGraphRenderer small;
	VideoGraphRenderer large;

	// Small: 16 x 16 = 256 pixels, each channel a different ordering of all 256 values.
	Bytes smallIn = picture(16, 16);
	for (size_t px = 0; px < 256u; ++px) {
		smallIn[px * 4u + 0] = static_cast<unsigned char>(px);
		smallIn[px * 4u + 1] = static_cast<unsigned char>(255u - px);
		smallIn[px * 4u + 2] = static_cast<unsigned char>((px * 7u + 3u) & 255u);
	}
	// Large: 64 x 64 = 4096 pixels, above the table's size.
	Bytes largeIn = picture(64, 64);
	for (size_t px = 0; px < 4096u; ++px) {
		largeIn[px * 4u + 0] = static_cast<unsigned char>(px & 255u);
		largeIn[px * 4u + 1] = static_cast<unsigned char>((px * 5u + 1u) & 255u);
		largeIn[px * 4u + 2] = static_cast<unsigned char>(255u - (px & 255u));
	}

	struct Setting {
		float drive, bias, fold, quantize, mirror, contrast, mix;
	};
	const Setting settings[] = {
		{1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f},            // the module's defaults
		{2.6f, -0.08f, 0.83f, 0.1f, 0.f, 1.1f, 0.85f},  // the Sounding Line patch
		{4.f, 1.f, 1.f, 0.f, 0.f, 2.f, 1.f},
		{0.f, -1.f, 0.f, 0.f, 0.f, 0.f, 1.f},
		{1.7f, 0.3f, 0.4f, 0.5f, 0.6f, 1.3f, 0.3f},
		{1.f, 0.f, 0.f, 1.f, 0.f, 1.f, 1.f},
		{3.3f, -0.6f, 0.f, 0.f, 1.f, 0.7f, 0.5f},
		{1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f},
		{2.2f, 0.45f, 0.65f, 0.02f, 0.15f, 1.9f, 0.999f},
	};
	size_t wrong = 0;
	size_t channelsDisagree = 0;
	size_t alphaWrong = 0;
	int count = 0;
	for (const Setting& s : settings) {
		WaveShaperParams p;
		p.drive = s.drive;
		p.bias = s.bias;
		p.fold = s.fold;
		p.quantize = s.quantize;
		p.mirror = s.mirror;
		p.contrast = s.contrast;
		p.mode = 0;
		p.mix = s.mix;
		videoBusPublishWaveShaper(kBusOut, kBusA, p);

		a.publish(smallIn, 16, 16);
		const Bytes refOut = render(small, kBusOut, 16, 16);
		if (refOut.size() != smallIn.size()) {
			check(false, "the small Wave Shaper picture rendered");
			return;
		}
		unsigned char ref[256];
		for (size_t px = 0; px < 256u; ++px) {
			ref[smallIn[px * 4u]] = refOut[px * 4u];
		}
		for (size_t px = 0; px < 256u; ++px) {
			if (refOut[px * 4u + 1] != ref[smallIn[px * 4u + 1]] || refOut[px * 4u + 2] != ref[smallIn[px * 4u + 2]]) ++channelsDisagree;
			if (refOut[px * 4u + 3] != 255) ++alphaWrong;
		}

		a.publish(largeIn, 64, 64);
		const Bytes out = render(large, kBusOut, 64, 64);
		if (out.size() != largeIn.size()) {
			check(false, "the large Wave Shaper picture rendered");
			return;
		}
		for (size_t i = 0; i < out.size(); i += 4u) {
			for (size_t c = 0; c < 3u; ++c) {
				if (out[i + c] != ref[largeIn[i + c]]) ++wrong;
			}
			if (out[i + 3] != 255) ++alphaWrong;
		}
		++count;
	}
	std::printf("  %d settings, 256 values each in three channels: %zu bytes differ\n", count, wrong);
	check(wrong == 0, "the Wave Shaper table gives the pixel loop's bytes for every value and setting");
	check(channelsDisagree == 0, "the Wave Shaper in RGB mode shapes each channel by its own byte alone");
	check(alphaWrong == 0, "Wave Shaper alpha is 255");
	videoBusClear(kBusOut);
}

}  // namespace

int main() {
	testBusMixer();
	testWaveShaper();
	std::printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
