#include "plugin.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"
#include "video_external_frame.hpp"
#include "video_player_core.hpp"
#include "video_player_engine.hpp"

#include <osdialog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// Video Player: plays a local video file, or a YouTube link downloaded with yt-dlp, and puts the
// picture on the video bus. The slow work (file access, yt-dlp, decoding, the playback clock) lives
// in video_player::Engine on its own thread (src/video_player_engine.hpp). This file is only the
// module wiring and the panel. The audio thread calls the engine's lock-free setters, never
// anything that waits.

struct VideoPlayer : Module {
	enum ParamId {
		OPEN_PARAM,
		PLAY_PARAM,
		STOP_PARAM,
		LOOP_PARAM,
		SPEED_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		PLAY_INPUT,
		STOP_INPUT,
		LOOP_INPUT,
		SPEED_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		VIDEO_BUS_OUTPUT,
		END_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		PLAY_LIGHT,
		LOOP_LIGHT,
		LIGHTS_LEN
	};

	VideoBusOwner videoBus;
	int externalFrameId = 0;
	std::string cacheDir;
	std::unique_ptr<video_player::Engine> engine;

	// What the engine was last told, for the status display. Written by the audio thread, read by
	// the UI thread.
	std::atomic<bool> loopOn {false};
	std::atomic<float> speedNow {1.f};

	dsp::BooleanTrigger playButton;
	dsp::BooleanTrigger stopButton;
	dsp::SchmittTrigger playInput;
	dsp::SchmittTrigger stopInput;
	dsp::SchmittTrigger loopInput;
	dsp::PulseGenerator endPulse;
	uint32_t endSeen = 0;

	VideoPlayer() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configButton(OPEN_PARAM, "Open video file");
		configButton(PLAY_PARAM, "Play / pause");
		configButton(STOP_PARAM, "Stop (back to the first frame)");
		configSwitch(LOOP_PARAM, 0.f, 1.f, 0.f, "Loop", {"Off: stop at the end, hold the last frame", "On: repeat"});
		// Speed is stored as a power of two so that 1x sits at a natural place on the knob and one volt
		// at the speed input doubles the speed. The display shows the speed itself.
		configParam(SPEED_PARAM, -2.f, 1.f, 0.f, "Speed", "x", 2.f, 1.f);
		configInput(PLAY_INPUT, "Play / pause trigger");
		configInput(STOP_INPUT, "Stop trigger");
		configInput(LOOP_INPUT, "Loop gate (high = repeat)");
		configInput(SPEED_INPUT, "Speed (1 V per doubling)");
		configOutput(VIDEO_BUS_OUTPUT, "Video bus token");
		configOutput(END_OUTPUT, "End of clip trigger");
		configLight(PLAY_LIGHT, "Playing");
		configLight(LOOP_LIGHT, "Loop on");

		cacheDir = asset::user("VideoResearchUnit-video-cache");
		externalFrameId = videoExternalFrameAllocate();
		video_player::EngineOptions options;
		options.cacheDir = cacheDir;
		engine.reset(new video_player::Engine(externalFrameId, options));
	}

	~VideoPlayer() override {
		// Stop the worker (and any yt-dlp it started) before the frame slot goes away.
		engine.reset();
		videoExternalFrameRelease(externalFrameId);
	}

	void process(const ProcessArgs& args) override {
		// A press of the button and a rising edge at the jack do the same thing. Both triggers are
		// always updated, so neither misses an edge.
		const bool playPressed = playButton.process(params[PLAY_PARAM].getValue() > 0.5f);
		const bool playTriggered = playInput.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 2.f);
		if (playPressed || playTriggered) {
			engine->requestPlayToggle();
		}
		const bool stopPressed = stopButton.process(params[STOP_PARAM].getValue() > 0.5f);
		const bool stopTriggered = stopInput.process(inputs[STOP_INPUT].getVoltage(), 0.1f, 2.f);
		if (stopPressed || stopTriggered) {
			engine->requestStop();
		}

		// Loop is on when the button is lit or the gate is high.
		loopInput.process(inputs[LOOP_INPUT].getVoltage(), 0.1f, 2.f);
		const bool loop = params[LOOP_PARAM].getValue() > 0.5f || loopInput.isHigh();
		engine->setLoop(loop);
		loopOn.store(loop, std::memory_order_relaxed);

		float octaves = params[SPEED_PARAM].getValue();
		if (inputs[SPEED_INPUT].isConnected()) {
			octaves += inputs[SPEED_INPUT].getVoltage();
		}
		const float speed = std::exp2(clamp(octaves, -2.f, 1.f));
		engine->setSpeed(speed);
		speedNow.store(speed, std::memory_order_relaxed);

		// One trigger pulse each time the clip ends, including each wrap while looping.
		const uint32_t endNow = engine->endCount();
		if (endNow != endSeen) {
			endSeen = endNow;
			endPulse.trigger(0.01f);
		}
		outputs[END_OUTPUT].setVoltage(endPulse.process(args.sampleTime) ? 10.f : 0.f);

		// Picture: the pixels stay in the external frame slot, the bus only carries its id.
		const int busId = videoBus.id();
		if (externalFrameId > 0 && engine->hasFrame()) {
			videoBusPublishExternalFrame(busId, externalFrameId);
		}
		else {
			videoBusClear(busId);
		}
		outputs[VIDEO_BUS_OUTPUT].setVoltage(videoBus.voltage());

		lights[PLAY_LIGHT].setBrightnessSmooth(engine->isPlaying() ? 1.f : 0.f, args.sampleTime);
		lights[LOOP_LIGHT].setBrightnessSmooth(loop ? 1.f : 0.f, args.sampleTime);
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		engine->clearSource();
	}

	// The patch keeps the bus id and where the video comes from. Playback never starts by itself
	// when a patch loads.
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		saveVideoBusId(rootJ, videoBus);
		const video_player::KeyValues values = video_player::settingsToKeyValues(engine->settings());
		for (const auto& entry : values) {
			json_object_set_new(rootJ, entry.first.c_str(), json_string(entry.second.c_str()));
		}
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		loadVideoBusId(rootJ, videoBus, "outputBusVoltage");

		video_player::KeyValues values;
		for (const char* key : {"source", "filePath", "youtubeUrl"}) {
			json_t* valueJ = json_object_get(rootJ, key);
			if (valueJ && json_is_string(valueJ)) {
				values[key] = json_string_value(valueJ);
			}
		}
		engine->restore(video_player::settingsFromKeyValues(values));
	}
};

namespace {

// Opens the system file dialog and loads the chosen video. UI thread only.
void chooseVideoFile(VideoPlayer* player) {
	if (!player) {
		return;
	}
	std::string startDir;
	const video_player::Settings saved = player->engine->settings();
	if (saved.source == video_player::SourceKind::File && !saved.filePath.empty()) {
		startDir = system::getDirectory(saved.filePath);
	}
	// MKV and WebM are listed on purpose: picking one gives a clear message instead of nothing.
	osdialog_filters* filters = osdialog_filters_parse("Video:mp4,m4v,mov,mkv,webm");
	char* path = osdialog_file(OSDIALOG_OPEN, startDir.empty() ? nullptr : startDir.c_str(), nullptr, filters);
	osdialog_filters_free(filters);
	if (path) {
		player->engine->loadFile(path);
		std::free(path);
	}
}

// Shortens text with "..." until it fits maxWidth at the current font size. Cuts on UTF-8
// character boundaries.
std::string videoPlayerFit(NVGcontext* vg, const std::string& text, float maxWidth) {
	if (nvgTextBounds(vg, 0.f, 0.f, text.c_str(), nullptr, nullptr) <= maxWidth) {
		return text;
	}
	std::string kept = text;
	while (!kept.empty()) {
		kept.pop_back();
		while (!kept.empty() && (static_cast<unsigned char>(kept.back()) & 0xC0) == 0x80) {
			kept.pop_back();
		}
		const std::string candidate = kept + "...";
		if (nvgTextBounds(vg, 0.f, 0.f, candidate.c_str(), nullptr, nullptr) <= maxWidth) {
			return candidate;
		}
	}
	return "...";
}

// Breaks text into lines of at most maxChars characters, on spaces where possible. A single word
// longer than a line is cut.
std::vector<std::string> videoPlayerWrap(const std::string& text, size_t maxChars) {
	std::vector<std::string> lines;
	std::string line;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find(' ', pos);
		if (end == std::string::npos) {
			end = text.size();
		}
		std::string word = text.substr(pos, end - pos);
		pos = end + 1;
		while (word.size() > maxChars) {
			if (!line.empty()) {
				lines.push_back(line);
				line.clear();
			}
			lines.push_back(word.substr(0, maxChars));
			word = word.substr(maxChars);
		}
		if (word.empty()) {
			continue;
		}
		if (!line.empty() && line.size() + 1 + word.size() > maxChars) {
			lines.push_back(line);
			line.clear();
		}
		if (!line.empty()) {
			line += ' ';
		}
		line += word;
	}
	if (!line.empty()) {
		lines.push_back(line);
	}
	return lines;
}

const char* videoPlayerStateWord(video_player::TransportState state) {
	using video_player::TransportState;
	switch (state) {
		case TransportState::NoSource: return "NO VIDEO";
		case TransportState::Loading: return "LOADING";
		case TransportState::Ready: return "READY";
		case TransportState::Playing: return "PLAYING";
		case TransportState::Paused: return "PAUSED";
		case TransportState::Ended: return "ENDED";
		case TransportState::Error: return "ERROR";
	}
	return "";
}

// Layout of the readout that both its drawing and the seek strip over the progress bar depend on.
constexpr float kReadoutPad = 6.f;  // margin left and right of the text and the progress bar
constexpr float kBarY = 19.5f;      // top of the progress bar
constexpr float kBarH = 2.5f;       // height of the progress bar

// The readout under the header. Top line: what the player is doing and the clock. Then a progress
// bar (VideoPlayerSeekArea lies over it, so a click or drag there moves around in the video), the
// file or link name, the status message (up to three lines; the right-click menu has the whole
// text), and at the bottom the picture size, loop state and speed.
struct VideoPlayerDisplay : TransparentWidget {
	VideoPlayer* module = nullptr;

	void draw(const DrawArgs& args) override {
		using video_player::TransportState;
		NVGcontext* vg = args.vg;
		const float w = box.size.x;
		const float h = box.size.y;
		const float pad = kReadoutPad;
		const float textWidth = w - 2.f * pad;
		const NVGcolor lit = nvgRGB(190, 255, 170);
		const NVGcolor dim = nvgRGB(210, 210, 210);
		const NVGcolor faint = nvgRGB(160, 166, 170);
		const NVGcolor amber = nvgRGB(255, 214, 128);
		const NVGcolor alert = nvgRGB(255, 140, 128);

		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0.f, 0.f, w, h, 3.f);
		nvgFillColor(vg, nvgRGB(8, 10, 13));
		nvgFill(vg);

		video_player::Snapshot snap;
		bool loop = false;
		float speed = 1.f;
		if (module && module->engine) {
			snap = module->engine->snapshot();
			loop = module->loopOn.load(std::memory_order_relaxed);
			speed = module->speedNow.load(std::memory_order_relaxed);
		}
		const bool hasClip = snap.state == TransportState::Ready || snap.state == TransportState::Playing ||
			snap.state == TransportState::Paused || snap.state == TransportState::Ended;

		auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (font) {
			nvgFontFaceId(vg, font->handle);
		}

		// Line 1: state word on the left, clock on the right.
		NVGcolor stateColor = dim;
		if (snap.state == TransportState::Playing) {
			stateColor = lit;
		}
		else if (snap.state == TransportState::Loading) {
			stateColor = amber;
		}
		else if (snap.state == TransportState::Error) {
			stateColor = alert;
		}
		nvgFontSize(vg, 11.5f);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, stateColor);
		nvgText(vg, pad, 10.f, videoPlayerStateWord(snap.state), nullptr);

		if (hasClip) {
			// Elapsed time grows leftwards from a fixed "/ total", so its right edge does not jitter.
			// If the clip is long enough that the full clock would run into the state word, the
			// "/ total" is left out for the whole clip (the bar still shows how far along it is).
			// The room needed is judged with the clip's own length, so the choice never flips mid-play.
			const std::string elapsed = video_player::formatClock(snap.positionSeconds);
			std::string total;
			if (snap.durationSeconds > 0.0) {
				total = "/ " + video_player::formatClock(snap.durationSeconds);
			}
			float totalWidth = 0.f;
			if (!total.empty()) {
				const float stateWidth = nvgTextBounds(vg, 0.f, 0.f, videoPlayerStateWord(snap.state), nullptr, nullptr);
				const std::string widest = video_player::formatClock(snap.durationSeconds);
				const float widestWidth = nvgTextBounds(vg, 0.f, 0.f, widest.c_str(), nullptr, nullptr);
				totalWidth = nvgTextBounds(vg, 0.f, 0.f, total.c_str(), nullptr, nullptr);
				if (widestWidth + 4.f + totalWidth > textWidth - stateWidth - 8.f) {
					total.clear();
					totalWidth = 0.f;
				}
			}
			float rightEdge = w - pad;
			if (!total.empty()) {
				nvgFillColor(vg, faint);
				nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
				nvgText(vg, rightEdge - totalWidth, 10.f, total.c_str(), nullptr);
				rightEdge -= totalWidth + 4.f;
			}
			nvgFillColor(vg, dim);
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgText(vg, rightEdge, 10.f, elapsed.c_str(), nullptr);
		}

		// Progress bar.
		if (hasClip && snap.durationSeconds > 0.0) {
			const float barY = kBarY;
			const float barH = kBarH;
			const float fraction = clamp(static_cast<float>(snap.positionSeconds / snap.durationSeconds), 0.f, 1.f);
			nvgBeginPath(vg);
			nvgRoundedRect(vg, pad, barY, textWidth, barH, 1.2f);
			nvgFillColor(vg, nvgRGB(52, 57, 62));
			nvgFill(vg);
			nvgBeginPath(vg);
			nvgRoundedRect(vg, pad, barY, std::max(barH, textWidth * fraction), barH, 1.2f);
			nvgFillColor(vg, lit);
			nvgFill(vg);
		}

		// Line 2: file name or link.
		nvgFontSize(vg, 10.f);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, snap.sourceName.empty() ? faint : dim);
		const std::string name = snap.sourceName.empty() ? std::string("No video loaded") : snap.sourceName;
		nvgText(vg, pad, 31.f, videoPlayerFit(vg, name, textWidth).c_str(), nullptr);

		// Status message, or a hint while nothing is loaded.
		std::string message = snap.message;
		NVGcolor messageColor = snap.messageIsError ? alert : dim;
		if (message.empty() && snap.state == TransportState::NoSource) {
			message = "Press OPEN FILE, or right-click this module to use a YouTube link.";
			messageColor = faint;
		}
		if (!message.empty()) {
			const int maxRows = 8;
			const int shownRows = 3;
			NVGtextRow rows[maxRows];
			const int rowCount = nvgTextBreakLines(vg, message.c_str(), nullptr, textWidth, rows, maxRows);
			nvgFillColor(vg, messageColor);
			for (int i = 0; i < rowCount && i < shownRows; i++) {
				const float y = 43.f + 11.5f * static_cast<float>(i);
				if (i == shownRows - 1 && rowCount > shownRows) {
					const std::string last(rows[i].start, rows[i].end);
					nvgText(vg, pad, y, videoPlayerFit(vg, last + "...", textWidth).c_str(), nullptr);
				}
				else {
					nvgText(vg, pad, y, rows[i].start, rows[i].end);
				}
			}
		}

		// Bottom line: picture size, loop state, speed.
		nvgFontSize(vg, 9.f);
		nvgFillColor(vg, faint);
		const float bottomY = h - 7.f;
		if (snap.width > 0 && snap.height > 0) {
			const std::string size = std::to_string(snap.width) + "x" + std::to_string(snap.height);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgText(vg, pad, bottomY, size.c_str(), nullptr);
		}
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgText(vg, w * 0.5f, bottomY, loop ? "LOOP ON" : "LOOP OFF", nullptr);
		char speedText[24];
		std::snprintf(speedText, sizeof(speedText), "%.2fx", static_cast<double>(speed));
		nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		nvgText(vg, w - pad, bottomY, speedText, nullptr);

		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0.5f, 0.5f, w - 1.f, h - 1.f, 3.f);
		nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 46));
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
	}
};

// A strip over the progress bar in the readout. A left click on it jumps to that point in the video,
// and dragging goes on seeking, also after the pointer leaves the strip (the bar ends are the start
// and the end of the clip).
//
// It is a sibling added after the readout rather than a child of it, so it gets events first:
// VideoPlayerDisplay is a TransparentWidget, which does not pass events on to its children. It takes
// only a plain left press while a video is loaded. Everything else (right-click for the menu, a press
// with Shift, Ctrl, Alt or Cmd held, a press while nothing is loaded) is left alone and reaches the
// module as before, so the module can still be dragged by its readout.
struct VideoPlayerSeekArea : Widget {
	// Null in the module browser, where there is no module to seek in.
	VideoPlayer* module = nullptr;
	// Where the pointer is along the strip, in this widget's units. Kept from the pointer's movement
	// since the press, because during a drag the pointer may be anywhere on screen.
	float dragX = 0.f;
	// The last target sent to the engine. Frames with no pointer movement send nothing.
	double lastSeconds = -1.0;

	// True when a video is loaded, and then gives its length in seconds.
	bool clipLoaded(double* durationSeconds = nullptr) const {
		using video_player::TransportState;
		if (!module || !module->engine) {
			return false;
		}
		const video_player::Snapshot snap = module->engine->snapshot();
		const bool hasClip = snap.state == TransportState::Ready || snap.state == TransportState::Playing ||
			snap.state == TransportState::Paused || snap.state == TransportState::Ended;
		if (!hasClip || !(snap.durationSeconds > 0.0)) {
			return false;
		}
		if (durationSeconds) {
			*durationSeconds = snap.durationSeconds;
		}
		return true;
	}

	// Sends the engine the time that lies at x along the progress bar.
	void seekToX(float x) {
		double duration = 0.0;
		if (!clipLoaded(&duration)) {
			return;
		}
		const float run = box.size.x - 2.f * kReadoutPad;
		if (!(run > 0.f)) {
			return;
		}
		const double fraction = clamp((x - kReadoutPad) / run, 0.f, 1.f);
		const double seconds = fraction * duration;
		if (std::fabs(seconds - lastSeconds) < 1e-6) {
			return;
		}
		lastSeconds = seconds;
		module->engine->requestSeek(seconds);
	}

	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && (e.mods & RACK_MOD_MASK) == 0 &&
			clipLoaded()) {
			dragX = e.pos.x;
			lastSeconds = -1.0;
			seekToX(dragX);
			// Consuming the press makes this the dragged widget, which brings onDragMove.
			e.consume(this);
			return;
		}
		Widget::onButton(e);
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (e.button != GLFW_MOUSE_BUTTON_LEFT) {
			return;
		}
		// mouseDelta is in window pixels and does not know about the zoom of the rack. It is also zero
		// on frames where the pointer did not move.
		const float zoom = getAbsoluteZoom();
		if (!(zoom > 0.f)) {
			return;
		}
		dragX += e.mouseDelta.x / zoom;
		seekToX(dragX);
	}
};

// Bezel button that opens the file dialog when it is released.
struct VideoPlayerOpenButton : VCVBezel {
	void onDragEnd(const DragEndEvent& e) override {
		VCVBezel::onDragEnd(e);
		if (e.button != GLFW_MOUSE_BUTTON_LEFT) {
			return;
		}
		engine::ParamQuantity* pq = getParamQuantity();
		if (pq && pq->module) {
			chooseVideoFile(static_cast<VideoPlayer*>(pq->module));
		}
	}
};

// Text field in the right-click menu. Enter loads what was typed or pasted, then closes the menu.
struct VideoPlayerLinkField : ui::TextField {
	VideoPlayer* player = nullptr;
	bool focused = false;

	void step() override {
		// Take the keyboard once, so the link can be typed or pasted straight away.
		if (!focused) {
			focused = true;
			APP->event->setSelectedWidget(this);
		}
		TextField::step();
	}

	void onSelectKey(const SelectKeyEvent& e) override {
		if (e.action == GLFW_PRESS && (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
			if (player && player->engine && !text.empty()) {
				player->engine->loadLink(text);
			}
			if (ui::MenuOverlay* overlay = getAncestorOfType<ui::MenuOverlay>()) {
				overlay->requestDelete();
			}
			e.consume(this);
			return;
		}
		TextField::onSelectKey(e);
	}
};

}  // namespace

struct VideoPlayerWidget : ModuleWidget {
	VideoPlayer* player = nullptr;

	VideoPlayerWidget(VideoPlayer* module) {
		using namespace rasterlab_panel;
		const float W = 60.96f;  // 12 HP
		setModule(module);
		player = module;
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VideoPlayer.svg")));
		addPanelHeader(this, W, "07", "VIDEO PLAYER");

		auto* display = new VideoPlayerDisplay();
		display->module = module;
		display->box.pos = mm2px(Vec(5.f, 18.5f));
		display->box.size = mm2px(Vec(W - 10.f, 30.f));
		addChild(display);

		// A strip over the progress bar: click or drag on it to move around in the video. Added after
		// the readout so it gets events first. It reaches 4 px above the bar and 3.5 px below it.
		auto* seekArea = new VideoPlayerSeekArea();
		seekArea->module = module;
		seekArea->box.pos = display->box.pos.plus(Vec(0.f, kBarY - 4.f));
		seekArea->box.size = Vec(display->box.size.x, 10.f);
		addChild(seekArea);

		// One table: the control on the left, its name in the middle, its jack on the right. A row
		// with a second line says what the light or the knob position means.
		const float controlX = 9.f;
		const float jackX = W - 9.f;
		const float labelX = W * 0.5f;
		const NVGcolor captionColor = nvgRGB(90, 90, 88);
		// Captions follow LABEL_MM but stop growing at 3.6 mm, so long captions still fit between the
		// control and the jack, about 43 mm apart. This fit is estimated from the layout, not measured in Rack.
		const float captionMm = std::min(LABEL_MM, 3.6f);
		auto rowLabel = [&](float y, const std::string& label, const std::string& caption) {
			if (caption.empty()) {
				addChild(createPanelText(Vec(labelX, y), label, LABEL_MM));
			}
			else {
				addChild(createPanelText(Vec(labelX, y - 2.5f), label, LABEL_MM));
				addChild(createPanelText(Vec(labelX, y + 2.5f), caption, captionMm, captionColor));
			}
		};

		// Source.
		const float openY = 57.f;
		addParam(createParamCentered<VideoPlayerOpenButton>(mm2px(Vec(controlX, openY)), module, VideoPlayer::OPEN_PARAM));
		rowLabel(openY, "OPEN FILE", "");

		// Transport.
		const float playY = 68.5f;
		addParam(createLightParamCentered<VCVLightBezel<GreenLight>>(mm2px(Vec(controlX, playY)), module, VideoPlayer::PLAY_PARAM, VideoPlayer::PLAY_LIGHT));
		rowLabel(playY, "PLAY / PAUSE", "LIT = PLAYING");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jackX, playY)), module, VideoPlayer::PLAY_INPUT));

		const float stopY = 79.5f;
		addParam(createParamCentered<VCVBezel>(mm2px(Vec(controlX, stopY)), module, VideoPlayer::STOP_PARAM));
		rowLabel(stopY, "STOP", "BACK TO START");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jackX, stopY)), module, VideoPlayer::STOP_INPUT));

		const float loopY = 90.5f;
		addParam(createLightParamCentered<VCVLightBezelLatch<YellowLight>>(mm2px(Vec(controlX, loopY)), module, VideoPlayer::LOOP_PARAM, VideoPlayer::LOOP_LIGHT));
		rowLabel(loopY, "LOOP", "LIT = REPEAT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jackX, loopY)), module, VideoPlayer::LOOP_INPUT));

		// Speed.
		const float speedY = 105.5f;
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(controlX, speedY)), module, VideoPlayer::SPEED_PARAM));
		rowLabel(speedY, "SPEED", "1x = NORMAL");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(jackX, speedY)), module, VideoPlayer::SPEED_INPUT));

		// Outputs. BUS is the video bus output, labelled the same way on every VRU module.
		addPlatedOutput<PJ301MPort>(this, Vec(W * 0.5f - 13.f, 119.f), "END", module, VideoPlayer::END_OUTPUT);
		addPlatedOutput<PJ301MPort>(this, Vec(W * 0.5f + 13.f, 119.f), "BUS", module, VideoPlayer::VIDEO_BUS_OUTPUT);
	}

	void appendContextMenu(ui::Menu* menu) override {
		if (!player) {
			return;
		}
		VideoPlayer* p = player;
		const video_player::Settings saved = p->engine->settings();
		const video_player::Snapshot snap = p->engine->snapshot();

		menu->addChild(new ui::MenuSeparator);
		menu->addChild(createMenuLabel("Video source"));
		menu->addChild(createMenuItem("Open video file...", "", [=]() {
			chooseVideoFile(p);
		}));

		menu->addChild(createMenuLabel("YouTube link (press Enter to load)"));
		auto* field = new VideoPlayerLinkField();
		field->player = p;
		field->box.size.x = 260.f;
		field->placeholder = "Paste a YouTube link here";
		menu->addChild(field);
		menu->addChild(createMenuItem("Load the YouTube link on the clipboard", "", [=]() {
			const char* clip = glfwGetClipboardString(APP->window->win);
			if (clip) {
				p->engine->loadLink(clip);
			}
		}));

		menu->addChild(createMenuItem("Remove video", "", [=]() {
			p->engine->clearSource();
		}, saved.source == video_player::SourceKind::None));
		menu->addChild(createMenuItem("Open folder of downloaded videos", "", [=]() {
			system::createDirectories(p->cacheDir);
			system::openDirectory(p->cacheDir);
		}));

		if (!snap.message.empty()) {
			menu->addChild(new ui::MenuSeparator);
			menu->addChild(createMenuLabel("Status"));
			for (const std::string& line : videoPlayerWrap(snap.message, 44)) {
				menu->addChild(createMenuLabel(line));
			}
		}
	}
};

Model* modelVideoPlayer = createModel<VideoPlayer, VideoPlayerWidget>("VideoPlayer");
