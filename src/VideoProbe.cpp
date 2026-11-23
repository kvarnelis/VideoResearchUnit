#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"
#include "render_service.hpp"
#include "video_probe_math.hpp"
#include "video_probe_layout.hpp"
#include "panel_items.hpp"
#include "video_clock.hpp"
#include "slow_param.hpp"

#include <atomic>
#include <memory>
#include <vector>

// VRU-26 Video Probe: a picture in, voltages out. A read point that X and Y move over the picture gives the
// brightness and colour under it at every audio sample (LUMA, R, G, B, 0 to 10 V). AVG, MOTION and GATE are
// whole-picture meters that change once per picture. No video bus out.
//
// How the picture reaches process(): the module's widget (VideoProbeFrameWidget) asks the render service for the bus
// at a fixed 320 x 240 (video_probe::kW x kH) and turns it into summed-area tables (video_probe_math.hpp). The tables
// go to the audio thread through a triple buffer: no locks, no allocation, and process() only reads the latest
// complete picture. The widget registers with the service from step(), which Rack calls every UI frame whether or
// not the widget is drawn, and takes the finished picture there (docs/architecture.md, "Render service").
//   - The picture is as fresh as Rack's UI frame (60 times a second by default) and one picture late.
//   - It keeps updating while the module is scrolled out of the Rack window, at 30 pictures a second when nothing
//     at all is drawn. It stops when Rack stops stepping its widgets (the window minimised, if Rack does that),
//     when the module is bypassed, when the bus has no video, and when the plugin setting "Keep video running when
//     hidden" is off; then the widget takes the picture in draw() as before, and process() holds the last values
//     whenever the widget is not drawn.
struct VideoProbe : Module {
	enum ParamId { X_PARAM, Y_PARAM, SIZE_PARAM, SMOOTH_PARAM, LEVEL_PARAM, X_AMT_PARAM, Y_AMT_PARAM, SCAN_PARAM, LINES_PARAM, SLOW_PARAM, PARAMS_LEN };
	enum InputId { VIDEO_BUS_INPUT, X_INPUT, Y_INPUT, INPUTS_LEN };
	enum OutputId { LUMA_OUTPUT, R_OUTPUT, G_OUTPUT, B_OUTPUT, AVG_OUTPUT, MOTION_OUTPUT, GATE_OUTPUT, FRAME_OUTPUT, OUTPUTS_LEN };
	enum LightId { SIGNAL_LIGHT, LIGHTS_LEN };

	// Read by the widget.
	std::atomic<int> selectedBus {1};
	std::atomic<uint32_t> frameCount {0};     // pictures published so far
	std::atomic<float> shownX {0.f}, shownY {0.f}, shownSize {0.f};   // where the probe is reading, for the marker

	// Hand-off of finished pictures from the widget (writer) to process() (reader).
	std::unique_ptr<video_probe::Frame[]> frames;
	video_probe::TripleBuffer handOff;

	// process() state.
	video_probe::Reader reader;
	vclock::Clock scanClock;   // SCAN: the video clock for the current LINES and sample rate
	float liveTimer = 0.f;
	uint32_t lastFrameCount = 0;
	bool live = false;

	void describe(int paramId, const char* text) { paramQuantities[paramId]->description = text; }

	VideoProbe() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		frames.reset(new video_probe::Frame[3]());
		configParam(X_PARAM, -5.f, 5.f, 0.f, "X position", " V");
		configParam(Y_PARAM, -5.f, 5.f, 0.f, "Y position", " V");
		configParam(SIZE_PARAM, 0.f, 1.f, 0.f, "Size", "%", 0.f, 100.f);
		configParam(SMOOTH_PARAM, 0.f, 1.f, 0.f, "Smooth", "%", 0.f, 100.f);
		configParam(LEVEL_PARAM, 0.f, 1.f, .5f, "Gate level", "%", 0.f, 100.f);
		configParam(X_AMT_PARAM, -1.f, 1.f, 1.f, "X jack amount", "%", 0.f, 100.f);
		configParam(Y_AMT_PARAM, -1.f, 1.f, 1.f, "Y jack amount", "%", 0.f, 100.f);
		configSwitch(SCAN_PARAM, 0.f, 1.f, 0.f, "Scan the picture as a raster", {"Off: read point from X and Y", "On: axes with no cable follow the video clock"});
		auto* lines = configParam(LINES_PARAM, static_cast<float>(raster_scan::kMinLines), static_cast<float>(raster_scan::kMaxLines), static_cast<float>(raster_scan::kDefaultLines), "Scan lines");
		lines->snapEnabled = true;
		configSlow(this, SLOW_PARAM);
		describe(SCAN_PARAM, "On: an axis with no cable in its jack follows the video clock instead of the knob, so with neither X nor Y patched the whole picture is read as a raster, left to right and top to bottom, once per picture (see LINES). Each sample is the average of the cell of picture it stands for. At SLOW x1 a line lasts 63.55 us, only about 3 samples at 48 kHz, so the picture reaches the outputs as about 3 columns; SLOW makes lines K times longer for K times the columns (sharper means slower). Between pictures the flyback reads black. Off: the read point is X and Y, as always.");
		describe(LINES_PARAM, "How many lines a scanned picture has, the same meaning as Scan Sync's LINES and Raster's. A picture takes (LINES + 22.5) lines of time (times SLOW). Only used when SCAN is on; match it to the module you are patching to.");
		describe(X_PARAM, "Where the read point sits across the picture. -5 V is the left edge, +5 V the right edge. The X jack is added to this.");
		describe(Y_PARAM, "Where the read point sits up the picture. -5 V is the bottom edge, +5 V the top edge. The Y jack is added to this.");
		describe(SIZE_PARAM, "How big a patch of the picture is read. All the way down reads one point. Turned up, LUMA, R, G and B (and the gate) are the average of a square patch around the point, up to the picture's full height.");
		describe(SMOOTH_PARAM, "Slows LUMA, R, G and B so jumps glide. All the way down is no slowing; all the way up takes about a second to settle. Turn it down for audio.");
		describe(LEVEL_PARAM, "How bright the patch under the read point must be for GATE to go high. GATE has a small margin: it turns on a little above this and off a little below.");
		describe(X_AMT_PARAM, "How much of the X jack is used. Full is 1 V of jack moves the point 1 V across the picture; negative flips it.");
		describe(Y_AMT_PARAM, "How much of the Y jack is used. Full is 1 V of jack moves the point 1 V up the picture; negative flips it.");
		configInput(VIDEO_BUS_INPUT, "Video bus token");
		configInput(X_INPUT, "X: moves the read point across the picture (5 V is a half picture width). Fast signals turn the picture into a sound.");
		configInput(Y_INPUT, "Y: moves the read point up the picture (5 V is a half picture height)");
		configOutput(LUMA_OUTPUT, "Brightness under the read point (0 to 10 V)");
		configOutput(R_OUTPUT, "Red under the read point (0 to 10 V)");
		configOutput(G_OUTPUT, "Green under the read point (0 to 10 V)");
		configOutput(B_OUTPUT, "Blue under the read point (0 to 10 V)");
		configOutput(AVG_OUTPUT, "Average brightness of the whole picture (0 to 10 V, changes once per picture)");
		configOutput(MOTION_OUTPUT, "How much the picture changed since the last one (0 to 10 V; 10 V is an average change of 12.5% of full brightness)");
		configOutput(GATE_OUTPUT, "10 V while the brightness under the read point is above LEVEL, otherwise 0 V");
		configOutput(FRAME_OUTPUT, "A 10 V pulse, 1 ms long, each time a new picture arrives (about 60 a second). LUMA, R, G and B change only at or after the pulse, so it can clock a sample and hold or a sequencer to the picture.");
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		reader.reset();
	}

	void process(const ProcessArgs& args) override {
		using namespace video_probe;
		const int busId = inputs[VIDEO_BUS_INPUT].isConnected() ? videoBusIdFromVoltage(inputs[VIDEO_BUS_INPUT].getVoltage()) : 1;
		selectedBus.store(busId, std::memory_order_relaxed);

		const bool scan = params[SCAN_PARAM].getValue() > .5f;
		const bool scanX = scan && !inputs[X_INPUT].isConnected();
		const bool scanY = scan && !inputs[Y_INPUT].isConnected();
		if (scan) {
			const int lines = raster_scan::clampLines(params[LINES_PARAM].getValue());
			const int slow = vclock::slowFromStep(params[SLOW_PARAM].getValue());
			if (scanClock.visibleLines() != lines || scanClock.slow() != slow || scanClock.sampleRate() != static_cast<double>(args.sampleRate)) {
				scanClock = vclock::Clock(static_cast<double>(args.sampleRate), lines, slow);
			}
		}
		const float xv = position(params[X_PARAM].getValue(), params[X_AMT_PARAM].getValue(), inputs[X_INPUT].getVoltage());
		const float yv = position(params[Y_PARAM].getValue(), params[Y_AMT_PARAM].getValue(), inputs[Y_INPUT].getVoltage());
		const float size = sane(params[SIZE_PARAM].getValue(), 0.f, 1.f);
		float markX = xv, markY = yv;
		if (scanX || scanY) {
			// The marker shows where this sample sits on the picture (as volts), so it sweeps while scanning.
			const raster_scan::Cell c = raster_scan::cellAt(scanClock, args.frame);
			if (scanX) markX = static_cast<float>(c.u * 10.0 - 5.0);
			if (scanY) markY = static_cast<float>(5.0 - (c.line + .5) * 10.0 / scanClock.visibleLines());
		}
		shownX.store(markX, std::memory_order_relaxed);
		shownY.store(markY, std::memory_order_relaxed);
		shownSize.store(size, std::memory_order_relaxed);

		const Reader::Out o = scan
			? reader.processScan(handOff, frames.get(), scanClock, args.frame, scanX, scanY, xv, yv, size, params[SMOOTH_PARAM].getValue(),
				params[LEVEL_PARAM].getValue(), args.sampleTime)
			: reader.process(handOff, frames.get(), xv, yv, size, params[SMOOTH_PARAM].getValue(),
				params[LEVEL_PARAM].getValue(), args.sampleTime);
		if (o.valid) {
			outputs[AVG_OUTPUT].setVoltage(o.avg);
			outputs[MOTION_OUTPUT].setVoltage(o.motion);
		}
		outputs[LUMA_OUTPUT].setVoltage(o.luma);
		outputs[R_OUTPUT].setVoltage(o.r);
		outputs[G_OUTPUT].setVoltage(o.g);
		outputs[B_OUTPUT].setVoltage(o.b);
		outputs[GATE_OUTPUT].setVoltage(o.gate);
		outputs[FRAME_OUTPUT].setVoltage(o.frame);

		// SIG: lit while pictures keep arriving (checked ten times a second).
		liveTimer += args.sampleTime;
		if (liveTimer >= .1f) {
			liveTimer = 0.f;
			const uint32_t n = frameCount.load(std::memory_order_relaxed);
			live = n != lastFrameCount;
			lastFrameCount = n;
		}
		lights[SIGNAL_LIGHT].setBrightnessSmooth(live ? 1.f : 0.f, args.sampleTime);
	}
};

// Renders the bus at the probe's own size, hands the picture to process(), and draws it with the read point on it.
// Rack calls draw() once per redraw while the widget is on screen. That is the only place the picture is made.
struct VideoProbeFrameWidget : TransparentWidget {
	VideoProbe* module = nullptr;
	video_probe::Analyzer analyzer;
	int nvgImage = 0;
	bool haveImage = false;

	VideoProbeFrameWidget() = default;
	~VideoProbeFrameWidget() override {
		renderService().forget(this);
	}

	// Analyses one finished picture and hands it to process().
	void ingest(const unsigned char* rgba) {
		using namespace video_probe;
		Frame& slot = module->frames[module->handOff.writeIndex()];
		analyzer.analyze(rgba, slot);
		module->handOff.publish();
		module->frameCount.fetch_add(1, std::memory_order_relaxed);
	}

	// Runs every UI frame, drawn or not. Registers the bus with the render service, which draws it for us when no
	// draw() does, and takes the newest picture it has not given us yet.
	void step() override {
		using namespace video_probe;
		TransparentWidget::step();
		if (!module) {
			return;
		}
		const RenderService::Taken taken = renderService().touch(this, module->selectedBus.load(std::memory_order_relaxed), kW, kH, true);
		if (!taken.fresh) {
			return;
		}
		if (taken.signal && taken.pixels->size() == static_cast<size_t>(kW) * kH * 4u) {
			ingest(taken.pixels->data());
		}
		else {
			analyzer.reset();   // after a gap, the next picture has nothing to compare with
		}
	}

	void draw(const DrawArgs& args) override {
		using namespace video_probe;
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(6, 8, 12));
		nvgFill(args.vg);
		if (!module) {
			drawLabel(args, "NO MODULE");
			return;
		}

		const RenderService::Result picture = renderService().frame(this, module->selectedBus.load(std::memory_order_relaxed), kW, kH, true);
		const std::vector<unsigned char>& rgba = *picture.pixels;
		const bool ok = picture.signal && rgba.size() == static_cast<size_t>(kW) * kH * 4u;
		// With "Keep video running when hidden" on, step() takes every picture, drawn or not. Off, only draw() does.
		const bool drawTakes = !renderService().feedsFromStep();
		if (ok) {
			if (drawTakes) {
				ingest(rgba.data());
			}
			if (!nvgImage) nvgImage = nvgCreateImageRGBA(args.vg, kW, kH, 0, rgba.data());
			else if (picture.fresh) nvgUpdateImage(args.vg, nvgImage, rgba.data());
			haveImage = true;
		}
		else if (drawTakes && !picture.pending) {
			analyzer.reset();   // after a gap, the next picture has nothing to compare with
		}
		if (haveImage && nvgImage) {
			NVGpaint paint = nvgImagePattern(args.vg, 0.f, 0.f, box.size.x, box.size.y, 0.f, nvgImage, (ok || picture.pending) ? 1.f : .35f);
			nvgBeginPath(args.vg);
			nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
			nvgFillPaint(args.vg, paint);
			nvgFill(args.vg);
		}
		if (!ok && !picture.pending) drawLabel(args, "NO VIDEO");
		drawMarker(args);

		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f);
		nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 46));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);
	}

	// The patch being read (a square, outlined dark then light so it shows on any picture) and a cross at its centre.
	void drawMarker(const DrawArgs& args) {
		using namespace video_probe;
		const float cx = static_cast<float>(pixelX(module->shownX.load(std::memory_order_relaxed))) / kW * box.size.x;
		const float cy = static_cast<float>(pixelY(module->shownY.load(std::memory_order_relaxed))) / kH * box.size.y;
		const float hx = halfWidthPx(module->shownSize.load(std::memory_order_relaxed)) / kW * box.size.x;
		const float hy = halfWidthPx(module->shownSize.load(std::memory_order_relaxed)) / kH * box.size.y;
		const float arm = std::max(2.f, mm2px(1.2f));
		nvgSave(args.vg);
		nvgScissor(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		for (int pass = 0; pass < 2; ++pass) {
			nvgBeginPath(args.vg);
			nvgRect(args.vg, cx - hx, cy - hy, hx * 2.f, hy * 2.f);
			nvgMoveTo(args.vg, cx - arm, cy); nvgLineTo(args.vg, cx + arm, cy);
			nvgMoveTo(args.vg, cx, cy - arm); nvgLineTo(args.vg, cx, cy + arm);
			nvgStrokeColor(args.vg, pass == 0 ? nvgRGBA(0, 0, 0, 200) : nvgRGBA(255, 235, 120, 255));
			nvgStrokeWidth(args.vg, pass == 0 ? 2.5f : 1.f);
			nvgStroke(args.vg);
		}
		nvgRestore(args.vg);
	}

	void drawLabel(const DrawArgs& args, const char* label) {
		rasterlab_panel::useLabelFont(args.vg);
		nvgFontSize(args.vg, mm2px(rasterlab_panel::LABEL_MM));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, nvgRGB(196, 196, 196));
		nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, label, nullptr);
	}

	void onRemove(const RemoveEvent& e) override {
		if (nvgImage && APP && APP->window) {
			nvgDeleteImage(APP->window->vg, nvgImage);
			nvgImage = 0;
		}
		TransparentWidget::onRemove(e);
	}
};

struct VideoProbeWidget : ModuleWidget {
	VideoProbeWidget(VideoProbe* module) {
		using namespace rasterlab_panel;
		constexpr float W = kVideoProbePanelWidth;   // 20 HP
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VideoProbe.svg")));
		addPanelHeader(this, W, "26", "VIDEO PROBE");

		auto* screen = new VideoProbeFrameWidget();
		screen->module = module;
		screen->box.size = mm2px(Vec(kVideoProbePreview[2], kVideoProbePreview[3]));   // 4:3 like the 320 x 240 picture
		screen->box.pos = mm2px(Vec(kVideoProbePreview[0], kVideoProbePreview[1]));
		addChild(screen);

		PANEL_PLACE_ALL(VIDEO_PROBE_LAYOUT)
		quickenSlowKnob(this, VideoProbe::SLOW_PARAM);
	}
};

Model* modelVideoProbe = createModel<VideoProbe, VideoProbeWidget>("VideoProbe");
