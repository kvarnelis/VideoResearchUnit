#pragma once

#include "crt_beam.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

enum class VideoPattern : int {
	Oscillator = 0,
	Smpte75 = 1,
	Ebu75 = 2,
	Ebu100 = 3,
	Grayscale = 4,
	Grid = 5,
	BarsAndRows = 6,
};

// Bars and Rows (pattern 6): two oscillators, A and B, run against a television scan. Each pixel row
// takes one whole scanline, 240 visible lines of a 262.5-line field, at the line rate in crt_beam.hpp.
// See docs/architecture.md, "Bars and Rows".
constexpr int kPairLines = crt::kDefaultLines;
constexpr double kPairFieldLines = crt::kDefaultLines + crt::kBlankingLines;
constexpr double kPairMinCycles = 0.25;
constexpr double kPairMaxCycles = 512.0;

struct VideoParams {
	float phase = 0.f;
	float frequency = 4.f;
	float hue = 0.f;
	float feedback = 0.f;
	float shape = 0.5f;
	float saturation = 1.f;
	float brightness = 1.f;
	float contrast = 1.f;
	int pattern = static_cast<int>(VideoPattern::Oscillator);
	// Bars and Rows only. A's wave is `shape` above. Cycles count per picture width on BARS and per field
	// on ROWS. A's phase moves by `bend` times B's value (0..1), in A's cycles. `mix` is 0 A, 1 A + B,
	// 2 A - B, 3 A x B. A locked oscillator starts every line at the same phase; a free one starts from
	// the phase latched at the top of the field (latchedA and latchedB, in cycles, 0..1), and `fieldOdd`
	// tells the renderer which of two interlaced fields it is drawing.
	float cyclesA = 8.f;
	float cyclesB = 4.f;
	float waveB = 0.f;
	float bend = 0.f;
	int mix = 1;
	float spread = 0.33f;
	bool barsA = true;
	bool barsB = false;
	bool lockA = true;
	bool lockB = true;
	bool fieldOdd = false;
	double latchedA = 0.0;
	double latchedB = 0.0;
};

// The scan clock for Bars and Rows. The module advances it once per audio sample. A free oscillator
// keeps running between fields; at each field boundary the phase is latched, and the renderer draws
// the field from the latched phase so a picture that repeats every field stands still.
struct PairClock {
	double fieldLine = 0.0;   // lines into the field, 0..kPairFieldLines
	double phaseA = 0.0;      // running phase, 0..1
	double phaseB = 0.0;
	double latchedA = 0.0;    // phase at the start of the current field
	double latchedB = 0.0;
	bool fieldOdd = false;    // flips at every field boundary

	static double cyclesPerLine(double cycles, bool bars) {
		return bars ? cycles : cycles / kPairFieldLines;
	}
	static double wrap(double value) {
		const double wrapped = value - std::floor(value);
		return wrapped >= 1.0 ? 0.0 : wrapped;
	}

	void advance(double dt, double cyclesA, bool barsA, double cyclesB, bool barsB) {
		if (!(dt > 0.0 && dt < 1.0)) {
			return;
		}
		const double lines = crt::kLineRateHz * dt;
		const double cplA = cyclesPerLine(cyclesA, barsA);
		const double cplB = cyclesPerLine(cyclesB, barsB);
		phaseA = wrap(phaseA + cplA * lines);
		phaseB = wrap(phaseB + cplB * lines);
		fieldLine += lines;
		int crossed = 0;
		while (fieldLine >= kPairFieldLines) {
			fieldLine -= kPairFieldLines;
			++crossed;
		}
		if (crossed > 0) {
			// The boundary fell fieldLine lines before this sample; take the phase back to it.
			latchedA = wrap(phaseA - fieldLine * cplA);
			latchedB = wrap(phaseB - fieldLine * cplB);
			if (crossed & 1) {
				fieldOdd = !fieldOdd;
			}
		}
	}
};

// One value, 0..1, of a Bars and Rows oscillator at `phase` (cycles). WAVE 0..1 runs sine, triangle at
// 1/3, ramp at 2/3, square at 1. `footprint` is the phase the oscillator moves across one pixel; no
// edge is made narrower than that, so a steep wave softens instead of aliasing.
float pairWaveValue(double phase, float wave, double footprint);

// DeflectionYoke deflection is computed at audio rate in the module and stored in its signal ring
// (see docs/architecture.md, "Scan-time signal model"). The renderer reads these channels at the
// scan time of each pixel.
enum DeflectionYokeSignalChannel : int {
	kDeflectionYokeDeflectX = 0,  // horizontal beam deflection, in normalized image units (+/-0.5 at full scale)
	kDeflectionYokeDeflectY = 1,  // vertical beam deflection, same units
	kDeflectionYokeZoom = 2,      // yoke amplitude term: scale = 1 + value about the image centre
	kDeflectionYokeSCoil = 3,     // S coil: the total deflection rotates by this value times deflection_yoke::kSCoilRadians
};

enum class DeflectionYokeEdge : int {
	Clamp = 0,
	Wrap = 1,
	Black = 2,
};

struct DeflectionYokeParams {
	float depth = 0.f;       // scales twist and line curl (deflection itself is in the ring)
	float lineCurl = 0.f;    // -3..3 (the knob reaches +/-1, a CV can add more)
	float twist = 0.f;       // -3..3
	bool flipH = false;
	bool flipV = false;
	int edge = static_cast<int>(DeflectionYokeEdge::Clamp);
	int signalSlotId = 0;    // signal ring holding the DeflectionYokeSignalChannel values; 0 = none
	// CRT beam (see docs/architecture.md, "CRT beam layer"). Defaults match the module's defaults.
	int lines = 240;         // visible scanlines per field, 120..600 (crt::kDefaultLines)
	float spot = 0.20f;      // spot size across the stroke, in units of the scanline pitch
	float persist = 0.f;     // phosphor persistence, 0..1
	float glow = 0.f;        // glow (bloom) amount, 0..2
	// Raster size: the horizontal and vertical stock scan, 0..1. Below 1 only the normal scan shrinks
	// toward the centre; the coil deflection keeps its full strength (separate yokes). 0 collapses the
	// raster onto a line, both 0 onto a point (see docs/architecture.md, "DeflectionYoke").
	float hSize = 1.f;
	float vSize = 1.f;
};

// Scan Processor deflection, blank and beam Z are computed at audio rate in the module and stored in its signal
// ring. The renderer reads them at the scan time of each point (see docs/architecture.md, "Scan Processor").
enum ScanProcessorSignalChannel : int {
	kScanProcessorDeflectX = 0,  // horizontal deflection, in ramp units (0.5 at +5 V, so +5 V moves the picture half its width)
	kScanProcessorDeflectY = 1,  // vertical deflection, same units (scaled by V HEIGHT in the renderer)
	kScanProcessorBlank = 2,     // 1 while the beam is blanked (BLANK input above 1 V), else 0
	kScanProcessorZ = 3,         // beam brightness offset at audio rate: 0 leaves the beam alone, -1 turns it off, +1 doubles it
};

struct ScanProcessorParams {
	// Positions are normalized to the screen: 0..1 spans the picture, 1 is one full screen.
	// The beam lands at x = hPos + hWidth (ramp + DEFLECT X) and y = vPos + vHeight (line + DEFLECT Y) - lumaGain x luma.
	float lumaGain = 0.2f;   // vertical lift per unit of luma (bipolar), in screen heights
	float hPos = 0.f;        // horizontal offset (-1..1)
	float hWidth = 1.f;      // horizontal scale (-2..2): negative mirrors the picture about hPos
	float vPos = 0.f;        // vertical offset (-1..1)
	float vHeight = 1.f;     // vertical scale (0..2): 0 collapses every line onto one thin line
	int lines = 128;         // scanlines: 128, 256, 525 or 1050 (the field rate follows the count)
	float intensity = 0.6f;  // beam energy scale
	bool rotate = false;     // ROTATE 90: swap the axes so the lines run vertically
	bool color = false;      // beam colour from the source (true) or a green/white phosphor (false)
	float zIntensity = 0.f;  // Z-INTENSITY: beam brightens with luma, 0..1
	float persist = 0.35f;   // phosphor persistence, 0..1
	float glow = 0.25f;      // glow (bloom) amount, 0..2
	int signalSlotId = 0;    // signal ring holding the ScanProcessorSignalChannel values; 0 = none
};

struct LineFieldParams {
	float depth = 0.55f;
	float xTilt = 0.28f;
	float yTilt = 0.f;
	float zSpin = 0.f;
	float perspective = 0.35f;
	float lineDensity = 0.55f;
	float gain = 1.15f;
	float mix = 1.f;
	bool invert = false;
};

enum class ScopeTraceMode : int {
	XY = 0,
	Oscilloscope = 1,
	DualTrace = 2,
	Polar = 3,
};


struct ColorLabParams {
	float hue = 0.f;
	float saturation = 1.f;
	float brightness = 1.f;
	float contrast = 1.f;
	float posterize = 0.f;
	float solarize = 0.f;
	int channelMode = 0;
	bool invert = false;
	float mix = 1.f;
};

struct KeyerParams {
	float threshold = 0.5f;
	float softness = 0.12f;
	float gain = 1.f;
	int mode = 0;
	bool invert = false;
	float hue = 0.33f;
	float mix = 1.f;
};

struct BusMixerParams {
	int busB = 1;
	int matteBus = 1;
	float mix = 0.5f;
	int mode = 0;
	float matteAmount = 0.f;
	bool invertMatte = false;
};

struct WaveShaperParams {
	float drive = 1.f;
	float bias = 0.f;
	float fold = 0.f;
	float quantize = 0.f;
	float mirror = 0.f;
	float contrast = 1.f;
	int mode = 0;
	float mix = 1.f;
};

struct FreezeFrameParams {
	int freezeSlotId = 1;
	int captureCounter = 0;
	float hold = 0.f;
	float blend = 1.f;
	float strobe = 0.f;
	bool clear = false;
};

struct ScopeTraceParams {
	int mode = static_cast<int>(ScopeTraceMode::XY);
	float scaleX = 0.85f;
	float scaleY = 0.85f;
	float offsetX = 0.f;
	float offsetY = 0.f;
	float timebase = 0.35f;
	float thickness = 0.4f;  // BEAM: the width of the beam spot, sigma = 0.5 + 2.5 x this, in pixels at 480 rows
	float decay = 0.65f;
	float hue = 0.32f;
	float intensity = 1.2f;
	float rotation = 0.f;
	float bloom = 0.f;      // blur-add glow over the phosphor, 0..2
	// DWELL: how much a slow stretch of the trace brightens. 0 gives the same energy on every pixel of
	// the path; 1 gives the full beam-speed law (energy per pixel goes as 1/speed, relative to the mean
	// speed of the window, up to 16 times the even level). Values between blend the two.
	float dwell = 0.5f;
	bool triggerPatched = false;  // TRIG cable in use: Oscilloscope and Dual start on its rising edge
	// Level the Oscilloscope and Dual trigger crosses when no TRIG cable is patched, normalised like
	// the X channel: trigger level volts / 5. 0 is the original zero crossing.
	float triggerLevel = 0.f;
	// RESET counter. When it changes the node's persistence (the glowing trail) is wiped before the
	// next window is drawn. Only the change matters, not the value.
	uint32_t clearSerial = 0;
};

// Scope Trace beam width. BEAM 0..1 sets the sigma of the round beam spot: 0.5 + 2.5 x BEAM pixels
// on a 480-row frame. A taller frame (the Display goes up to 1080 rows, Syphon Out is 768 or 1080)
// scales the spot with its height, so the trace keeps the same share of the picture at every size.
// A frame under 480 rows keeps the 480-row size. The sigma never goes above kScopeBeamMaxSigmaPx.
constexpr int kScopeBeamReferenceRows = 480;
constexpr float kScopeBeamMaxSigmaPx = 8.f;

inline float scopeBeamScale(int heightPx) {
	return std::max(1.f, static_cast<float>(heightPx) / static_cast<float>(kScopeBeamReferenceRows));
}

inline float scopeBeamSigmaPx(float beam, int heightPx) {
	const float knob = std::clamp(beam, 0.f, 1.f);
	return std::min((0.5f + 2.5f * knob) * scopeBeamScale(heightPx), kScopeBeamMaxSigmaPx);
}

class PreviewVideoEngine {
public:
	const std::vector<unsigned char>& render(int width, int height, const VideoParams& params);
	// Frame produced by the most recent render() call.
	const std::vector<unsigned char>& output() const {
		return rgba;
	}

private:
	std::vector<unsigned char> rgba;
	std::vector<unsigned char> previous;
	int previousWidth = 0;
	int previousHeight = 0;

	struct Color {
		float r = 0.f;
		float g = 0.f;
		float b = 0.f;
	};

	void renderOscillator(int width, int height, const VideoParams& params);
	void renderSmpte75(int width, int height);
	void renderEbu75(int width, int height);
	void renderEbu100(int width, int height);
	void renderGrayscale(int width, int height);
	void renderGrid(int width, int height);
	void renderPair(int width, int height, const VideoParams& params);
	void applyColorControls(int width, int height, const VideoParams& params);
	void setPixel(int width, int x, int y, Color color);

	static float fract(float value);
	static float smoothstep(float edge0, float edge1, float value);
	static Color barColor(float r, float g, float b);
	static Color colorForBar(float xNorm, const std::vector<Color>& bars);
	static void hsvToRgb(float h, float s, float v, float& r, float& g, float& b);
};

struct ColorizerParams {
	float hue[4] = {0.66f, 0.86f, 0.08f, 0.15f};
	float level[4] = {0.25f, 0.5f, 0.75f, 1.f};
	float saturation = 0.9f;
	int bands = 0;
	float offset = 0.f;
};

struct RampsParams {
	int axisA = 0;
	float freqA = 1.f;
	float phaseA = 0.f;
	float waveA = 0.f;
	int axisB = 1;
	float freqB = 1.f;
	float phaseB = 0.f;
	float waveB = 0.f;
	int mix = 0;
};

// Shared by the bus renderer and the small panel preview; controls are sampled per frame.
void renderRamps(std::vector<unsigned char>& rgba, int width, int height, const RampsParams& params);

struct FeedbackParams {
	int ringSlotId = 0;
	int frameCounter = 0;
	int clearCounter = 0;
	float zoom = 1.f;
	float rotate = 0.f;
	float offsetX = 0.f;
	float offsetY = 0.f;
	int edge = 0;
	float hue = 0.f;
	float gain = 0.9f;
	float key = 0.f;
	int mode = 0;
};

// Rotator (VRU-20). Angle is in turns, already wrapped by rotatorAngle() in rotator_math.hpp; positive turns
// counter-clockwise on screen. Zoom, offsets and pivot follow the Feedback conventions (offsets and pivot are
// fractions of the frame width and height; the pivot is measured from the centre). Edge: 0 black, 1 hold,
// 2 repeat, 3 mirror. Folds: 1 is off, 2 to 12 mirror the picture into that many wedges about the pivot.
struct RotatorParams {
	float angle = 0.f;
	float zoom = 1.f;
	float offsetX = 0.f;
	float offsetY = 0.f;
	float pivotX = 0.f;
	float pivotY = 0.f;
	int edge = 0;
	int folds = 1;
};

// Shapes (VRU-21). One closed shape drawn as a distance picture: grey is brightness by distance from the
// shape's edge (mid grey on the edge, brighter inside, darker outside). Sides run 3 to 12 and may fall between
// whole numbers (the shape blends into the next polygon). Round 0..1 rounds the corners to a circle; point
// 0..1 pulls the middle of each side inward into a star. Size is the shape's inscribed radius in half picture
// heights. X and y are fractions of the frame width and height from the centre. Aspect is the stretch ratio
// (width over height, 0.25 to 4). Turn is in turns, positive counter-clockwise on screen. Repeat tiles the
// whole composition in square cells, that many to the picture height (1 is off). Copies puts that many shapes
// in a ring of radius ring (half picture heights, before Repeat). Melt 0..1 fuses neighbouring shapes. Spread
// is the grey ramp half-width in half picture heights (it is not scaled by Repeat). Matte draws plain white on
// black, one pixel of edge.
struct ShapesParams {
	float sides = 4.f;
	float round = 0.f;
	float point = 0.f;
	float size = 0.5f;
	float x = 0.f;
	float y = 0.f;
	float aspect = 1.f;
	float turn = 0.f;
	int repeat = 1;
	int copies = 1;
	float ring = 0.48f;
	float melt = 0.f;
	float spread = 0.12f;
	int matte = 0;
};

// Renders on several threads; each pixel depends only on the parameters, so the picture is the same however
// many threads run. Called by the bus renderer.
void renderShapes(std::vector<unsigned char>& rgba, int width, int height, const ShapesParams& params);

// Video Mixer (VRU-22). Four picture buses, A to D, of which one is live and one is next; fader is how far the
// next picture has come up over the live one (0 = live only, 1 = next only). A bus id of 0 means the picture is
// absent and reads as black. mode: 0 dissolve, 1 add, 2 wipe. A wipe sweeps a threshold through the greys of the
// pattern picture (or, with no pattern bus, through the next picture's own greys); soft is the width of its edge in
// grey levels. Colour: with colorOn[i] the picture i is read as grey and its brightness takes hue[i] (turns, any
// value, wrapped by the renderer; the SHIFT input is already added); sat is the strength of all colours, 0 = plain
// grey. The key lays keyBus over the mix (after the transition, before black): its hole is cut by matteBus when that
// is patched, else by keyBus itself, with the Keyer's threshold and softness; keyFill 0 shows the key picture in the
// hole, 1 a flat colour from keyHue. black fades the whole picture to black, last of all.
struct VideoMixerParams {
	int inputBus[4] = {0, 0, 0, 0};
	int onAir = 0;   // the live input, 0 to 3 for A to D (the code keeps this older name)
	int next = 1;
	int patternBus = 0;
	int keyBus = 0;
	int matteBus = 0;
	float fader = 0.f;
	int mode = 0;
	float soft = 0.15f;
	float black = 0.f;
	bool colorOn[4] = {false, false, false, false};
	float hue[4] = {0.f, 0.25f, 0.5f, 0.75f};
	float sat = 1.f;
	bool keyOn = false;
	float keyThresh = 0.5f;
	float keySoft = 0.12f;
	float keyHue = 0.33f;
	int keyFill = 0;
	bool keyInvert = false;
};
