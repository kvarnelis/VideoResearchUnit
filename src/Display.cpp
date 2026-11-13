#include "plugin.hpp"
#include "audio_ring.hpp"
#include "display_capture.hpp"
#include "display_recorder.hpp"
#include "display_select.hpp"
#include "panel_style.hpp"
#include "video_bus.hpp"
#include "video_graph_renderer.hpp"

#include <osdialog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
// Layout in mm, for a 3U panel. The screen is a 4:3 landscape picture tube shape (like a Trinitron),
// centred between a thin header and a thin bottom strip that holds the A/B/C jacks, SEL button and THRU output.
//
// The header is ONE line, not the four-line band the other VRU panels have: on a monitor the picture matters
// more than the nameplate, so the picture keeps its full size (141.7 x 106.3 mm at the default width) and the
// naming shares the thin strip above it. DISPLAY is centred on the panel at kTitleMm. VIDEO RESEARCH UNIT
// (SERIES_MM, grey) starts at the bezel's left edge and VRU–16 (NUMBER_MM) ends at the bezel's right edge, both
// placed from the screen's real edges at the current width. All three share the title's baseline (kSeriesYMm and
// kNumberYMm are the centre y that does it). There is no series rule on this panel.
// A panel too narrow for the series line to end kSeriesGapMm clear of the title hides the series line; the
// catalogue number always shows. See applyPanelHp.
constexpr float kScreenMarginMm = 3.f;
constexpr float kScreenTopMm = 8.f;
constexpr float kScreenBottomMm = 114.3f;
constexpr float kScreenWidthMm = 141.7f; // 4:3 with the full band height (106.3 mm)
constexpr float kScreenCornerMm = 1.5f;  // small rounded corners on the picture
constexpr float kBezelMm = 1.f;          // thin dark bezel around the picture
constexpr float kTitleMm = 4.6f;         // centre y of DISPLAY (TITLE_MM)
constexpr float kSeriesYMm = 5.18f;      // centre y of VIDEO RESEARCH UNIT (SERIES_MM): its baseline on the title's baseline
constexpr float kNumberYMm = 5.02f;      // centre y of VRU–16 (NUMBER_MM): same baseline
// Text widths measured in Nunito-Bold at the sizes in panel_style.hpp. They decide whether the series line fits
// beside the title, so remeasure them if SERIES_MM, TITLE_MM or the texts change. (VRU–16 at NUMBER_MM is 10.33 mm
// wide and never meets the title, so it has no constant.)
constexpr float kSeriesWidthMm = 25.63f; // SERIES_TEXT at SERIES_MM (2.8)
constexpr float kTitleWidthMm = 16.7f;   // "DISPLAY" at TITLE_MM (4.9)
constexpr float kSeriesGapMm = 3.f;      // least clear space between the end of the series line and the start of the title
constexpr float kStripMm = 119.8f;      // jack row of the bottom strip; the THRU plate (jack + 7.6 mm) ends 1.1 mm above the panel bottom
constexpr float kStripLabelDropMm = 0.4f; // A/B/C letters and the SEL label sit this far below PLATE_LABEL_MM, clear of the jacks

// SHOT and REC keep their outer-side labels. From 24 HP they sit on the strip row, flanking the A/B/C, SEL,
// THRU and L/R run, with the SHOT label's outer edge at the bezel. Narrower panels use the row above. (Before the
// audio jacks, 20 HP also had them beside; nine items at 20 HP would be 9 mm apart, less than a jack is wide.)
constexpr int kCaptureBesideMinHp = 24;
// The L and R audio jacks sit on the strip row from this width. At 12 HP they use a third row, above SHOT and REC.
constexpr int kAudioOnStripMinHp = 16;
constexpr float kAudioAboveMm = 99.f;
constexpr float kShotLabelWidthMm = 8.37153f; // SHOT advance at LABEL_MM (Nunito-Bold, hhea line height, as letterWidths)
constexpr float kCaptureAboveDxMm = 7.f;      // button centre from the panel centre, on the row above the strip
constexpr float kCaptureAboveMm = 108.6f;     // y of the row above the strip (buttons end 4 mm above the plates)
constexpr float kCaptureLabelDxMm = rasterlab_panel::R_BUTTON + rasterlab_panel::LABEL_GAP_MM;  // label edge from the button centre

// Cue timing, in seconds.
constexpr double kPictureWaitSeconds = 1.0;   // a press waits this long for a picture to draw, then fails
constexpr double kShotFlashSeconds = 0.25;    // white blink on the SHOT light: the screenshot was handed over
constexpr double kFailCueSeconds = 1.2;       // red pulses: a screenshot or recording failed
constexpr double kFailPulseSeconds = 0.4;     // one pulse is on for half of this
constexpr double kBadgeFailSeconds = 4.0;     // "REC FAILED" stays on the screen this long
constexpr double kRecPulseSeconds = 1.2;      // the REC light breathes at this period while recording
constexpr int kCloseWaitMs = 15000;           // longest wait for a recording to close when the module is deleted
constexpr double kTwoPi = 6.283185307179586;

double nowSeconds() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Smallest whole HP at which a panel of `mm` width fits.
constexpr int hpToFit(float mm, int hp = 1) {
	return hp * rasterlab_panel::HP_MM >= mm ? hp : hpToFit(mm, hp + 1);
}

// The full-height width: the 4:3 screen at full band height, plus the side margins, rounded up to whole HP.
constexpr int kFullHeightHp = hpToFit(kScreenWidthMm + 2.f * kScreenMarginMm);

// Panel widths the user can choose, in HP. The last entry is the full-height width and the default.
// Wider panels would only add side bands, so they are not offered.
constexpr int kHpChoices[] = {12, 16, 20, 24, kFullHeightHp};
constexpr int kDefaultHp = kFullHeightHp;

// Screen aspect (width : height) of the picture.
constexpr float kScreenAspect = 4.f / 3.f;

// Render size of the Display frame: rows follow the on-screen height, with a floor and a cap.
constexpr int kMinRenderLines = 480;
constexpr int kMaxRenderLines = 1080;
constexpr int kMaxRenderWidth = 2048;

// The choice nearest to hp (ties go to the narrower width). Patches saved with a width that is no
// longer offered load at the nearest one.
int snapHp(int hp) {
	int best = kHpChoices[0];
	for (int choice : kHpChoices) {
		if (std::abs(choice - hp) < std::abs(best - hp)) {
			best = choice;
		}
	}
	return best;
}

// Sets a widget's size and centre from mm values.
void placeMm(Widget* w, Vec centreMm, Vec sizeMm) {
	w->box.size = mm2px(sizeMm);
	w->box.pos = mm2px(centreMm) - w->box.size * 0.5f;
}

// Medium light that is white, red, or a mix. Reads two adjacent light ids.
template <typename TBase = GrayModuleLightWidget>
struct TWhiteRedLight : TBase {
	TWhiteRedLight() {
		this->addBaseColor(SCHEME_WHITE);
		this->addBaseColor(SCHEME_RED);
	}
};
using WhiteRedLight = TWhiteRedLight<>;
}

struct Display : Module {
	enum ParamId {
		SHOT_PARAM,
		REC_PARAM,
		SEL_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		BUS_A_INPUT,    // former VIDEO_BUS_INPUT, id 0 for saved patches
		BUS_B_INPUT,
		BUS_C_INPUT,
		AUDIO_L_INPUT,  // record only: the movie's left channel (appended, so saved cables keep their ids)
		AUDIO_R_INPUT,  // record only: the movie's right channel
		INPUTS_LEN
	};
	enum OutputId {
		THRU_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		SIGNAL_LIGHT,    // retired; keep id 0 for saved patches
		SHOT_LIGHT,      // white; the red half is the next id (SHOT_LIGHT_RED)
		SHOT_LIGHT_RED,
		REC_LIGHT,
		SEL_A_LIGHT,
		SEL_A_LIGHT_RED,
		SEL_B_LIGHT,
		SEL_B_LIGHT_RED,
		SEL_C_LIGHT,
		SEL_C_LIGHT_RED,
		LIGHTS_LEN
	};

	// What the badge on the screen shows.
	enum class RecBadge {
		None,
		Recording,
		Failed
	};

	std::atomic<int> selectedInput {0}; // audio thread and Input menu; saved in patch JSON
	dsp::BooleanTrigger selTrigger; // audio thread only
	std::atomic<int> selectedBus {1};
	std::atomic<bool> hasSignal {false};
	// Panel width in HP. Set from the context menu and saved in patch JSON. UI thread only.
	int panelHp = kDefaultHp;

	// ---- SHOT and REC ---------------------------------------------------------------------------
	//
	// Threads. process() runs on the audio thread and only counts button presses. Everything else runs on
	// the UI thread: uiTick() once per UI frame (from the widget's step()), onFrame() from the screen's
	// draw() with the picture that was just drawn. The UI thread copies the picture and nothing more. PNG
	// encoding and writing happen on the ShotWriter's thread, and the recorder paces, converts and
	// encodes on its own thread, so a press never waits for the disk or the encoder.
	//
	// No dialog ever opens from a button. The folder picker is in the right-click menu only.

	// Written by process(), read by the UI thread.
	std::atomic<unsigned> shotPresses {0};
	std::atomic<unsigned> recPresses {0};
	dsp::BooleanTrigger shotTrigger;  // audio thread only
	dsp::BooleanTrigger recTrigger;   // audio thread only

	// ---- Audio for REC ----
	//
	// The audio jacks feed the movie and nothing else. process() (audio thread) does three things with them,
	// none of which can block: it keeps `audioPatched` and `engineRate` current, and, only while a recording
	// with sound is running (`audioCapture`), pushes one stereo frame per call into the lock-free ring.
	// The ring is built on the UI thread when the first recording with sound starts and kept until the module
	// is destroyed, so the audio thread never sees it appear or disappear. The recorder's worker thread is
	// the one consumer (display_recorder.hpp, "Audio").
	std::atomic<unsigned> audioPatched {0};     // bit 0: L has a cable, bit 1: R has a cable
	std::atomic<float> engineRate {0.f};        // the engine sample rate, as process() last saw it
	std::atomic<bool> audioCapture {false};     // true from just after a recording with sound starts until it ends
	std::atomic<display_recorder::AudioRing*> audioRingPtr {nullptr};  // set once; points into audioRingKeep
	std::shared_ptr<display_recorder::AudioRing> audioRingKeep;        // UI thread; owns the ring

	// UI thread only from here on.
	// The folder for screenshots and videos. "" means the default (~/Pictures/Video Research Unit); so does a
	// folder that no longer exists. Saved in the patch.
	std::string captureFolder;

	unsigned handledShotPresses = 0;
	unsigned handledRecPresses = 0;
	bool pendingShot = false;      // SHOT pressed; waiting for the next picture the screen draws
	double pendingShotAt = 0.0;
	bool pendingRecStart = false;  // REC pressed; waiting for the first picture
	double pendingRecAt = 0.0;
	bool recordingOn = false;      // between the first REC press and the second (not saved in the patch)

	display_capture::ShotWriter shotWriter;
	std::unique_ptr<display_recorder::Session> session;                  // the recording in progress
	std::vector<std::unique_ptr<display_recorder::Session>> finishing;   // stopped; their files are being closed

	// Cue state. Times are nowSeconds(); a start time of -1e9 means "never".
	double shotOkAt = -1e9;
	double shotFailAt = -1e9;
	double recFailAt = -1e9;
	float recGlow = 0.f;           // brightness of the REC light, 0 to 1, shared with the screen badge
	bool badgeFailed = false;

	Display() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configButton(SHOT_PARAM, "Save screenshot");
		configButton(REC_PARAM, "Record video (press again to stop)");
		configButton(SEL_PARAM, "Select input (A, B, C)");
		configInput(BUS_A_INPUT, "Video bus A token");
		configInput(BUS_B_INPUT, "Video bus B token");
		configInput(BUS_C_INPUT, "Video bus C token");
		configInput(AUDIO_L_INPUT, "Audio left");
		configInput(AUDIO_R_INPUT, "Audio right, record only");
		configLight(SHOT_LIGHT, "Screenshot saved");
		configLight(SHOT_LIGHT_RED, "Screenshot failed");
		configLight(REC_LIGHT, "Recording");
		// A new Display starts with the folder the user picked last, in any patch.
		captureFolder = display_capture::lastPickedFolder();
	}

	// Closes any recording so its file is complete and playable. This runs when the module is deleted,
	// the patch is closed or Rack quits, always on the UI thread.
	~Display() override {
		if (session) {
			session->requestStop();
			finishing.push_back(std::move(session));
		}
		for (auto& recording : finishing) {
			recording->requestStop();
		}
		for (auto& recording : finishing) {
			if (recording->waitUntilDone(kCloseWaitMs)) {
				reportFinished(*recording);
			}
			else {
				WARN("Display: a recording did not finish in time: %s", recording->path().c_str());
			}
		}
		finishing.clear();
		// shotWriter's destructor writes any screenshots still queued and stops its thread.
	}

	void process(const ProcessArgs& args) override {
		int selection = selectedInput.load(std::memory_order_relaxed);
		if (selTrigger.process(params[SEL_PARAM].getValue() > 0.f)) {
			selection = displayNextInput(selection);
			selectedInput.store(selection, std::memory_order_relaxed);
		}
		const int inputId = BUS_A_INPUT + selection;
		const bool connected = inputs[inputId].isConnected();
		const float inputVoltage = connected ? inputs[inputId].getVoltage() : 0.f;
		outputs[THRU_OUTPUT].setVoltage(videoThruVoltage(connected, inputVoltage));
		const int busId = displayInputBusId(selection, connected, inputVoltage);
		const bool active = videoBusIsActive(busId);
		selectedBus.store(busId, std::memory_order_relaxed);
		hasSignal.store(active, std::memory_order_relaxed);
		for (int i = 0; i < 3; ++i) {
			lights[SEL_A_LIGHT + i * 2].setBrightnessSmooth(selection == i && active ? 1.f : 0.f, args.sampleTime);
			lights[SEL_A_LIGHT_RED + i * 2].setBrightnessSmooth(selection == i && !active ? 1.f : 0.f, args.sampleTime);
		}

		// Audio for REC. Nothing here locks, allocates or does I/O.
		const bool leftPatched = inputs[AUDIO_L_INPUT].isConnected();
		const bool rightPatched = inputs[AUDIO_R_INPUT].isConnected();
		const unsigned patched = (leftPatched ? 1u : 0u) | (rightPatched ? 2u : 0u);
		audioPatched.store(patched, std::memory_order_relaxed);
		engineRate.store(args.sampleRate, std::memory_order_relaxed);
		if (audioCapture.load(std::memory_order_acquire)) {
			if (display_recorder::AudioRing* ring = audioRingPtr.load(std::memory_order_acquire)) {
				float left = 0.f;
				float right = 0.f;
				display_recorder::routeAudio(patched, display_recorder::voltsToSample(inputs[AUDIO_L_INPUT].getVoltageSum()),
					display_recorder::voltsToSample(inputs[AUDIO_R_INPUT].getVoltageSum()), left, right);
				ring->setSampleRate(args.sampleRate);
				ring->push(left, right);  // a full ring drops the frame and counts it; it never waits
			}
		}

		// Presses are counted here, at audio rate, so even a very short click is seen. The UI thread acts on them.
		if (shotTrigger.process(params[SHOT_PARAM].getValue() > 0.f)) {
			shotPresses.fetch_add(1u, std::memory_order_relaxed);
		}
		if (recTrigger.process(params[REC_PARAM].getValue() > 0.f)) {
			recPresses.fetch_add(1u, std::memory_order_relaxed);
		}
	}

	int snapshotBusId() const {
		return selectedBus.load(std::memory_order_relaxed);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "panelHp", json_integer(panelHp));
		json_object_set_new(rootJ, "selectedInput", json_integer(selectedInput.load(std::memory_order_relaxed)));
		// Written even when empty, so "default folder" is told apart from a patch that predates the setting.
		json_object_set_new(rootJ, "captureFolder", json_string(captureFolder.c_str()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		const json_int_t savedInput = json_integer_value(json_object_get(rootJ, "selectedInput"));
		const int input = savedInput >= std::numeric_limits<int>::min() && savedInput <= std::numeric_limits<int>::max()
			? static_cast<int>(savedInput) : -1;
		selectedInput.store(displayClampInput(input), std::memory_order_relaxed);
		if (json_t* hpJ = json_object_get(rootJ, "panelHp")) {
			panelHp = snapHp(static_cast<int>(json_integer_value(hpJ)));
		}
		if (json_t* folderJ = json_object_get(rootJ, "captureFolder")) {
			if (json_is_string(folderJ)) {
				captureFolder = json_string_value(folderJ);
			}
		}
	}

	// ---- UI thread ------------------------------------------------------------------------------

	// Once per UI frame, whether or not the module is on screen.
	void uiTick() {
		const double now = nowSeconds();

		// Button presses counted by process(). Several in one frame (a stalled UI) are handled in order.
		const unsigned shots = shotPresses.load(std::memory_order_relaxed);
		if (shots != handledShotPresses) {
			handledShotPresses = shots;
			pendingShot = true;
			pendingShotAt = now;
		}
		const unsigned recs = recPresses.load(std::memory_order_relaxed);
		while (handledRecPresses != recs) {
			handledRecPresses++;
			toggleRecording(now);
		}

		// A press that never got a picture: the screen is not being drawn (scrolled away), or the signal is gone.
		if (pendingShot && now - pendingShotAt > kPictureWaitSeconds) {
			pendingShot = false;
			shotFailed(now, "no picture to save (is the Display on screen?)");
		}
		if (pendingRecStart && now - pendingRecAt > kPictureWaitSeconds) {
			pendingRecStart = false;
			recordingOn = false;
			recFailed(now, "no picture to record (is the Display on screen?)");
		}

		// Results from the worker threads.
		display_capture::ShotResult shot;
		while (shotWriter.pollResult(shot)) {
			if (shot.ok) {
				INFO("Display: screenshot saved to %s", shot.path.c_str());
			}
			else {
				shotFailed(now, shot.error);
			}
		}
		reapRecordings(now);

		updateLights(now);
	}

	// Called from the screen's draw() with the picture just drawn (RGBA, top row first). The pixels are
	// only valid during the call, so this copies what it needs.
	void onFrame(const unsigned char* rgba, int width, int height) {
		const double now = nowSeconds();
		if (pendingShot) {
			pendingShot = false;
			takeShot(now, rgba, width, height);
		}
		if (pendingRecStart) {
			pendingRecStart = false;
			startRecording(rgba, width, height);
		}
		else if (session) {
			session->submit(rgba, width, height);
		}
	}

	// Called from draw() when the screen shows no picture (no signal). A press waiting for a picture fails at once.
	void onNoSignal() {
		const double now = nowSeconds();
		if (pendingShot) {
			pendingShot = false;
			shotFailed(now, "no picture to save (NO VIDEO)");
		}
		if (pendingRecStart) {
			pendingRecStart = false;
			recordingOn = false;
			recFailed(now, "no picture to record (NO VIDEO)");
		}
	}

	RecBadge recBadge() const {
		if (recordingOn) {
			return RecBadge::Recording;
		}
		return badgeFailed ? RecBadge::Failed : RecBadge::None;
	}

	void takeShot(double now, const unsigned char* rgba, int width, int height) {
		if (!shotWriter.canAccept()) {
			shotFailed(now, "earlier screenshots are still being saved");
			return;
		}
		display_capture::ShotJob job;
		// This copy is all the UI thread does for a screenshot. Encoding and writing happen on the ShotWriter's thread.
		job.rgba.assign(rgba, rgba + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
		job.width = width;
		job.height = height;
		job.folder = captureFolder;
		job.stem = display_capture::timestampStem(std::time(nullptr));
		if (!shotWriter.submit(std::move(job))) {
			shotFailed(now, "earlier screenshots are still being saved");
			return;
		}
		shotOkAt = now;
	}

	void toggleRecording(double now) {
		if (recordingOn) {
			stopRecording();
			return;
		}
		if (!display_recorder::platformSupportsRecording()) {
			recFailed(now, "video recording needs macOS");
			return;
		}
		recFailAt = -1e9;
		recordingOn = true;
		pendingRecStart = true;
		pendingRecAt = now;
	}

	void startRecording(const unsigned char* rgba, int width, int height) {
		// The worker picks the file name and opens the encoder; it must not touch this module, so it gets copies.
		display_recorder::SessionOptions options;
		options.fps = display_recorder::Session::kFps;
		options.makeSink = []() { return display_recorder::createFrameSink(); };
		const std::string folder = captureFolder;
		const std::string stem = display_capture::timestampStem(std::time(nullptr));
		options.choosePath = [folder, stem](std::string& path, std::string& error) {
			return display_capture::chooseNewFilePath(folder, stem, "mov", path, error);
		};
		options.pathDone = [](const std::string& path) { display_capture::releasePath(path); };
		// Sound is recorded only if a cable is in L or R right now. The movie's rate is the engine's rate now.
		const float rate = engineRate.load(std::memory_order_relaxed);
		const bool withAudio = audioPatched.load(std::memory_order_relaxed) != 0u && rate >= 8000.f && rate <= 768000.f;
		if (withAudio) {
			if (!audioRingKeep) {
				audioRingKeep = std::make_shared<display_recorder::AudioRing>();
			}
			audioRingKeep->setSampleRate(rate);
			options.audio = audioRingKeep;
			options.audioSampleRate = static_cast<int>(rate + 0.5f);
		}
		session = std::make_unique<display_recorder::Session>(std::move(options), rgba, width, height);
		if (withAudio) {
			// The session has claimed the ring, so it is safe to let the audio thread start filling it.
			audioRingPtr.store(audioRingKeep.get(), std::memory_order_release);
			audioCapture.store(true, std::memory_order_release);
		}
	}

	// Second REC press. Returns at once: the worker writes the last frame and closes the file, and
	// reapRecordings() notices when it is done.
	void stopRecording() {
		recordingOn = false;
		pendingRecStart = false;
		audioCapture.store(false, std::memory_order_release);
		if (session) {
			session->requestStop();
			finishing.push_back(std::move(session));
		}
	}

	static void reportFinished(display_recorder::Session& recording) {
		const display_recorder::SessionStats stats = recording.stats();
		if (recording.state() == display_recorder::SessionState::Finished) {
			INFO("Display: recording saved to %s (%u frames, %u dropped, %u repeated)", recording.path().c_str(),
				static_cast<unsigned>(stats.written), static_cast<unsigned>(stats.dropped), static_cast<unsigned>(stats.repeated));
			if (stats.hasAudio) {
				INFO("Display: audio %llu frames, %llu lost, peak %.1f dBFS", static_cast<unsigned long long>(stats.audioFrames),
					static_cast<unsigned long long>(stats.audioDropped), stats.audioPeak > 0.f ? 20.0 * std::log10(stats.audioPeak) : -200.0);
				if (stats.audioPeak <= 0.f) {
					WARN("Display: the audio inputs carried no signal during this recording (every sample was zero), so the sound track is silent");
				}
			}
			if (stats.stoppedForRateChange) {
				WARN("Display: the recording was ended because the engine sample rate changed");
			}
		}
		else {
			WARN("Display: recording failed: %s", recording.error().c_str());
		}
	}

	void reapRecordings(double now) {
		// The recording in progress failed by itself (folder not writable, disk full, encoder error).
		if (session && session->state() == display_recorder::SessionState::Failed) {
			const std::string why = session->error();
			session.reset();
			recordingOn = false;
			audioCapture.store(false, std::memory_order_release);
			recFailed(now, why);
		}
		// The recorder ended itself: the engine sample rate changed while sound was being recorded. The file
		// is complete up to that point. Pressing REC again starts a new movie at the new rate.
		if (session && session->state() == display_recorder::SessionState::Finished) {
			recordingOn = false;
			audioCapture.store(false, std::memory_order_release);
			finishing.push_back(std::move(session));
		}
		for (size_t i = 0; i < finishing.size();) {
			const display_recorder::SessionState state = finishing[i]->state();
			if (state == display_recorder::SessionState::Finished) {
				reportFinished(*finishing[i]);
				finishing.erase(finishing.begin() + static_cast<std::ptrdiff_t>(i));
			}
			else if (state == display_recorder::SessionState::Failed) {
				const std::string why = finishing[i]->error();
				finishing.erase(finishing.begin() + static_cast<std::ptrdiff_t>(i));
				recFailed(now, why);
			}
			else {
				i++;
			}
		}
	}

	void shotFailed(double now, const std::string& why) {
		WARN("Display: screenshot failed: %s", why.c_str());
		shotFailAt = now;
	}

	void recFailed(double now, const std::string& why) {
		WARN("Display: recording failed: %s", why.c_str());
		recFailAt = now;
	}

	// SHOT light: a white blink when a screenshot was handed to the writer, red pulses when one failed.
	// REC light: lit the whole time a recording runs (it breathes between 45 % and 100 %, never off),
	// and a fast red flicker when a recording failed. The lights belong to the UI thread: process() never writes them.
	void updateLights(double now) {
		float white = 0.f;
		float red = 0.f;
		const double sinceShotFail = now - shotFailAt;
		if (sinceShotFail < kFailCueSeconds) {
			red = std::fmod(sinceShotFail, kFailPulseSeconds) < kFailPulseSeconds * 0.5 ? 1.f : 0.f;
		}
		else if (now - shotOkAt < kShotFlashSeconds) {
			white = 1.f;
		}
		lights[SHOT_LIGHT].setBrightness(white);
		lights[SHOT_LIGHT_RED].setBrightness(red);

		float rec = 0.f;
		const double sinceRecFail = now - recFailAt;
		if (recordingOn) {
			rec = 0.45f + 0.55f * (0.5f + 0.5f * static_cast<float>(std::sin(now * kTwoPi / kRecPulseSeconds)));
		}
		else if (sinceRecFail < kFailCueSeconds) {
			rec = std::fmod(sinceRecFail, kFailPulseSeconds) < kFailPulseSeconds * 0.5 ? 1.f : 0.f;
		}
		lights[REC_LIGHT].setBrightness(rec);
		recGlow = rec;
		badgeFailed = !recordingOn && sinceRecFail < kBadgeFailSeconds;
	}

	// ---- Folder (right-click menu) --------------------------------------------------------------

	// The folder a capture would go to now: the chosen one if it exists, else the default.
	std::string effectiveFolder() const {
		return display_capture::resolveCaptureFolder(captureFolder);
	}

	// Menu action. Opens the system folder picker, so it must only run from a menu click.
	void chooseCaptureFolder() {
		std::string start = effectiveFolder();
		if (!display_capture::directoryExists(start)) {
			start = display_capture::homeDirectory();
		}
		char* picked = osdialog_file(OSDIALOG_OPEN_DIR, start.empty() ? nullptr : start.c_str(), nullptr, nullptr);
		if (!picked) {
			return;
		}
		const std::string path = picked;
		std::free(picked);
		if (path.empty()) {
			return;
		}
		captureFolder = path;
		display_capture::setLastPickedFolder(path);
		INFO("Display: capture folder is now %s", path.c_str());
	}

	void useDefaultFolder() {
		captureFolder.clear();
		display_capture::setLastPickedFolder("");
	}

	void showCaptureFolder() {
		system::openDirectory(effectiveFolder());
	}
};

// Flat panel face. It covers the SVG panel so the width can change at run time.
struct DisplayFaceWidget : Widget {
	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(0xe8, 0xe8, 0xe6));
		nvgFill(args.vg);
	}
};

// Dark bezel behind the picture. Its box is the picture plus kBezelMm on each side.
struct DisplayBezelWidget : Widget {
	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, mm2px(kScreenCornerMm + kBezelMm));
		nvgFillColor(args.vg, nvgRGB(14, 14, 16));
		nvgFill(args.vg);
	}
};

struct DisplayFrameWidget : TransparentWidget {
	Display* module = nullptr;
	std::unique_ptr<VideoGraphRenderer> renderer;
	int nvgImage = 0;
	int imageWidth = 0;
	int imageHeight = 0;

	DisplayFrameWidget() {
		renderer = std::make_unique<VideoGraphRenderer>();
	}

	~DisplayFrameWidget() override = default;

	// Device pixels per Rack pixel: zoom times the window's pixel ratio (the larger of the two, so a
	// HiDPI display is not counted twice).
	float devicePixelScale() {
		const float ratio = APP && APP->window ? std::max(APP->window->pixelRatio, APP->window->windowRatio) : 1.f;
		return getAbsoluteZoom() * ratio;
	}

	// The picture is 4:3. Rows follow the on-screen height, clamped to [kMinRenderLines, kMaxRenderLines],
	// and width is four thirds of the rows, capped at kMaxRenderWidth.
	void renderSize(int& width, int& height) {
		const float wanted = box.size.y * devicePixelScale();
		height = std::clamp(static_cast<int>(std::lround(wanted)), kMinRenderLines, kMaxRenderLines);
		width = std::max(1, static_cast<int>(std::lround(height * kScreenAspect)));
		if (width > kMaxRenderWidth) {
			width = kMaxRenderWidth;
			height = std::max(1, static_cast<int>(std::lround(width / kScreenAspect)));
		}
	}

	// Rounded picture outline, used for the background, the image and the edge stroke.
	void picturePath(const DrawArgs& args) {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, mm2px(kScreenCornerMm));
	}

	void draw(const DrawArgs& args) override {
		drawPicture(args);
		drawRecBadge(args);
	}

	void drawPicture(const DrawArgs& args) {
		picturePath(args);
		nvgFillColor(args.vg, nvgRGB(4, 6, 9));
		nvgFill(args.vg);

		if (!module) {
			drawLabel(args, "NO VIDEO");
			drawFrame(args);
			return;
		}

		int width = 0;
		int height = 0;
		renderSize(width, height);
		const std::vector<unsigned char>& rgba = renderer->renderBus(module->snapshotBusId(), width, height);
		if (!renderer->hasSignal()) {
			module->onNoSignal();
			drawLabel(args, "NO VIDEO");
			drawFrame(args);
			return;
		}

		if (!rgba.empty() && rgba.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
			// The SHOT and REC buttons take their pictures here, from the renderer's buffer. Anything NanoVG
			// draws afterwards (labels, the REC badge, the frame stroke) is not in them. The renderer keeps
			// state between calls, so it is called once per draw and never from anywhere else.
			module->onFrame(rgba.data(), width, height);
			if (!nvgImage || width != imageWidth || height != imageHeight) {
				if (nvgImage) {
					nvgDeleteImage(args.vg, nvgImage);
				}
				nvgImage = nvgCreateImageRGBA(args.vg, width, height, 0, rgba.data());
				imageWidth = width;
				imageHeight = height;
			}
			else {
				nvgUpdateImage(args.vg, nvgImage, rgba.data());
			}
		}

		if (nvgImage) {
			NVGpaint paint = nvgImagePattern(args.vg, 0.f, 0.f, box.size.x, box.size.y, 0.f, nvgImage, 1.f);
			picturePath(args);
			nvgFillPaint(args.vg, paint);
			nvgFill(args.vg);
		}

		// The bus graph loops back on itself (or is too deep). The last good frame is shown.
		if (renderer->hasCycle()) {
			drawLabel(args, "CYCLE");
		}
		drawFrame(args);
	}

	void onRemove(const RemoveEvent& e) override {
		if (nvgImage && APP && APP->window) {
			nvgDeleteImage(APP->window->vg, nvgImage);
			nvgImage = 0;
		}
		TransparentWidget::onRemove(e);
	}

	void drawLabel(const DrawArgs& args, const char* label) {
		rasterlab_panel::useLabelFont(args.vg);
		nvgFontSize(args.vg, 13.f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, nvgRGB(196, 196, 196));
		nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, label, nullptr);
	}

	void drawFrame(const DrawArgs& args) {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f, mm2px(kScreenCornerMm));
		nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 40));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);
	}

	// A small badge in the top left corner of the picture: a red dot and "REC" while recording, and
	// "REC FAILED" for a few seconds after a recording failed. It is drawn here with NanoVG, over the
	// finished picture, so it is never part of a recorded frame or a screenshot.
	void drawRecBadge(const DrawArgs& args) {
		if (!module) {
			return;
		}
		const Display::RecBadge badge = module->recBadge();
		if (badge == Display::RecBadge::None) {
			return;
		}
		const bool failed = badge == Display::RecBadge::Failed;
		const char* text = failed ? "REC FAILED" : "REC";

		const float margin = 7.f;
		const float height = 17.f;
		const float padding = 6.f;
		const float dotRadius = 4.f;
		const float gap = 5.f;
		rasterlab_panel::useLabelFont(args.vg);
		nvgFontSize(args.vg, 11.f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		const float textWidth = nvgTextBounds(args.vg, 0.f, 0.f, text, nullptr, nullptr);
		const float width = padding + dotRadius * 2.f + gap + textWidth + padding;
		if (width + margin * 2.f > box.size.x) {
			return;
		}

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, margin, margin, width, height, height * 0.5f);
		nvgFillColor(args.vg, nvgRGBA(0, 0, 0, 170));
		nvgFill(args.vg);

		const float glow = failed ? 1.f : std::clamp(module->recGlow, 0.f, 1.f);
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, margin + padding + dotRadius, margin + height * 0.5f, dotRadius);
		nvgFillColor(args.vg, nvgRGBA(237, 44, 36, static_cast<unsigned char>(255.f * glow)));
		nvgFill(args.vg);

		nvgFillColor(args.vg, nvgRGB(240, 240, 240));
		nvgText(args.vg, margin + padding + dotRadius * 2.f + gap, margin + height * 0.5f, text, nullptr);
	}
};

namespace {
// Moves a widget so its centre sits at centreMm (for widgets whose size is known).
void centreAt(Widget* w, Vec centreMm) {
	w->box.pos = mm2px(centreMm) - w->box.size * 0.5f;
}

// Moves a label made by createPanelTextAt so its right edge (rightAligned) or left edge is at anchorMm.x
// and its vertical centre is at anchorMm.y.
void anchorTextAt(rasterlab_panel::PanelText* label, Vec anchorMm, bool rightAligned) {
	const Vec anchor = mm2px(anchorMm);
	label->box.pos = Vec(rightAligned ? anchor.x - label->box.size.x : anchor.x, anchor.y - label->box.size.y * 0.5f);
}
}

struct DisplayWidget : ModuleWidget {
	Display* displayModule = nullptr;
	int hp = kDefaultHp;

	DisplayFaceWidget* face = nullptr;
	rasterlab_panel::PanelText* seriesText = nullptr;
	rasterlab_panel::PanelText* numberText = nullptr;
	rasterlab_panel::PanelText* title = nullptr;
	Widget* bezel = nullptr;
	DisplayFrameWidget* screen = nullptr;
	rasterlab_panel::PanelText* inputLabels[3] {};
	Widget* busInputs[3] {};
	Widget* inputLights[3] {};
	ParamWidget* selButton = nullptr;
	rasterlab_panel::PanelText* selLabel = nullptr;
	rasterlab_panel::OutputPlate* thruPlate = nullptr;
	PJ301MPort* thruOut = nullptr;
	ParamWidget* shotButton = nullptr;
	Widget* shotLight = nullptr;
	rasterlab_panel::PanelText* shotLabel = nullptr;
	PortWidget* audioPorts[2] = {nullptr, nullptr};
	rasterlab_panel::PanelText* audioLabels[2] = {nullptr, nullptr};
	ParamWidget* recButton = nullptr;
	Widget* recLight = nullptr;
	rasterlab_panel::PanelText* recLabel = nullptr;

	DisplayWidget(Display* module) {
		using namespace rasterlab_panel;
		displayModule = module;
		setModule(module);
		// The SVG only sets the initial size and the browser preview. The face draws the panel.
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Display.svg")));

		face = new DisplayFaceWidget();
		addChild(face);

		// The one-line header (see the layout comment at the top of this file), built here as separate parts
		// instead of by addPanelHeader because the panel changes width: applyPanelHp places all three from the
		// screen's real edges. The series line is left-aligned and the number right-aligned, so each one
		// hangs from its own bezel edge whatever its width.
		seriesText = createPanelTextAt(Vec(0.f, kSeriesYMm), SERIES_TEXT, SERIES_MM, NVG_ALIGN_LEFT, nvgRGB(90, 90, 88));
		addChild(seriesText);
		numberText = createPanelTextAt(Vec(0.f, kNumberYMm), catalogueNumber("16"), NUMBER_MM, NVG_ALIGN_RIGHT);
		addChild(numberText);
		title = createPanelText(Vec(0.f, kTitleMm), "DISPLAY", TITLE_MM);
		addChild(title);

		bezel = new DisplayBezelWidget();
		addChild(bezel);

		screen = new DisplayFrameWidget();
		screen->module = module;
		addChild(screen);

		for (int i = 0; i < 3; ++i) {
			inputLabels[i] = createPanelTextAt(Vec(0.f, 0.f), std::string(1, 'A' + i), LABEL_MM, NVG_ALIGN_LEFT);
			addChild(inputLabels[i]);
			auto* port = createInputCentered<PJ301MPort>(Vec(0.f, 0.f), module, Display::BUS_A_INPUT + i);
			addInput(port);
			busInputs[i] = port;
			inputLights[i] = createLightCentered<SmallLight<GreenRedLight>>(Vec(0.f, 0.f), module, Display::SEL_A_LIGHT + i * 2);
			addChild(inputLights[i]);
		}
		// Audio for the movie: plain L and R jacks, labelled like A/B/C. Positions are set in applyPanelHp.
		for (int i = 0; i < 2; ++i) {
			audioLabels[i] = createPanelText(Vec(0.f, 0.f), i == 0 ? "L" : "R", LABEL_MM);
			addChild(audioLabels[i]);
			audioPorts[i] = createInputCentered<PJ301MPort>(Vec(0.f, 0.f), module, Display::AUDIO_L_INPUT + i);
			addInput(audioPorts[i]);
		}
		selButton = createParamCentered<LEDButton>(Vec(0.f, 0.f), module, Display::SEL_PARAM);
		addParam(selButton);
		selLabel = createPanelText(Vec(0.f, 0.f), "SEL", LABEL_MM);
		addChild(selLabel);

		// THRU: black plate with the jack on it, as on the other modules. The plate goes first so the jack draws on top.
		thruPlate = createOutputPlate(Vec(0.f, 0.f), "THRU");
		addChild(thruPlate);
		thruOut = createOutputCentered<PJ301MPort>(Vec(0.f, 0.f), module, Display::THRU_OUTPUT);
		addOutput(thruOut);

		// SHOT and REC: a button with its light over it (lights ignore the mouse, so the click reaches the
		// button), and the name on the outer side. The label's side is set in applyPanelHp.
		shotButton = createParamCentered<LEDButton>(Vec(0.f, 0.f), module, Display::SHOT_PARAM);
		addParam(shotButton);
		shotLight = createLightCentered<MediumLight<WhiteRedLight>>(Vec(0.f, 0.f), module, Display::SHOT_LIGHT);
		addChild(shotLight);
		shotLabel = createPanelTextAt(Vec(0.f, 0.f), "SHOT", LABEL_MM, NVG_ALIGN_RIGHT);
		addChild(shotLabel);

		recButton = createParamCentered<LEDButton>(Vec(0.f, 0.f), module, Display::REC_PARAM);
		addParam(recButton);
		recLight = createLightCentered<MediumLight<RedLight>>(Vec(0.f, 0.f), module, Display::REC_LIGHT);
		addChild(recLight);
		recLabel = createPanelTextAt(Vec(0.f, 0.f), "REC", LABEL_MM, NVG_ALIGN_LEFT);
		addChild(recLabel);

		applyPanelHp(module ? module->panelHp : kDefaultHp);
	}

	// Sets the panel width and positions every part for it. Called on load, on menu change and
	// whenever the module's saved width differs from the widget's.
	void applyPanelHp(int newHp) {
		using namespace rasterlab_panel;
		hp = snapHp(newHp);
		if (displayModule) {
			displayModule->panelHp = hp;
		}

		const float W = hp * HP_MM;
		const float cx = W * 0.5f;
		// Rack's addModule throws, and Rack aborts, unless a module is exactly RACK_GRID_HEIGHT (380 px)
		// tall. mm2px(128.5 mm) is 379.43 px, so size the box in grid units, never from millimetres.
		box.size = Vec(hp * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);
		face->box.pos = Vec(0.f, 0.f);
		face->box.size = box.size;

		// The picture is 4:3. It is as wide as the panel allows (up to kScreenWidthMm), and it sits
		// centred vertically in the band between the header and the bottom strip. On a narrow panel
		// that leaves equal space above and below.
		const float screenW = std::min(kScreenWidthMm, W - 2.f * kScreenMarginMm);
		const float screenH = screenW / kScreenAspect;
		const Vec screenCentre(cx, (kScreenTopMm + kScreenBottomMm) * 0.5f);
		placeMm(screen, screenCentre, Vec(screenW, screenH));
		const float bezelHalf = screenW * 0.5f + kBezelMm;
		placeMm(bezel, screenCentre, Vec(2.f * bezelHalf, screenH + 2.f * kBezelMm));

		// Header, one line. DISPLAY is centred; the series line hangs from the bezel's left edge and the
		// number from its right edge, both placed from the picture's real edges at this width. The series
		// line is hidden when its right end would come within kSeriesGapMm of the title's left end (15 HP and
		// narrower, with the measured widths); the number is 10.33 mm wide and never gets near the title.
		centreAt(title, Vec(cx, kTitleMm));
		anchorTextAt(seriesText, Vec(cx - bezelHalf, kSeriesYMm), false);
		anchorTextAt(numberText, Vec(cx + bezelHalf, kNumberYMm), true);
		seriesText->visible = (cx - bezelHalf) + kSeriesWidthMm + kSeriesGapMm <= cx - kTitleWidthMm * 0.5f;

		// The strip row holds, left to right: [SHOT] A B C SEL THRU [L R] [REC], at one pitch, centred on the panel.
		//  - 24 HP and wider: all nine on the strip row. The SHOT label's outer edge meets the bezel.
		//  - 16 and 20 HP: A B C SEL THRU L R on the strip row, the THRU plate's and R jack's outer edges at the
		//    bezel; SHOT and REC sit on the row above.
		//  - 12 HP: only A B C SEL THRU fit the strip row; SHOT and REC sit on the row above, and L and R on a
		//    third row above that (the picture is small enough there to leave the room).
		const bool beside = hp >= kCaptureBesideMinHp;
		const bool audioOnStrip = hp >= kAudioOnStripMinHp;
		const int slots = beside ? 4 : (audioOnStrip ? 3 : 2);  // pitches from the centre to the outermost jack
		const float pitch = beside ? (bezelHalf - kCaptureLabelDxMm - kShotLabelWidthMm) / static_cast<float>(slots)
			: (bezelHalf - PLATE_W_MM * 0.5f) / static_cast<float>(slots);
		// Labels share one centreline; each letter/light pair is centred under its jack.
		// Nunito-Bold advances at LABEL_MM (hhea line height), measured from Rack's font.
		constexpr float letterWidths[] = {2.16974f, 2.05445f, 2.01602f};
		const float labelY = kStripMm + PLATE_LABEL_MM + kStripLabelDropMm;
		// Slot of A: the run A B C SEL THRU is centred on the row, or shifted left to make room for L R.
		const int firstSlot = beside ? -3 : (audioOnStrip ? -3 : -2);
		for (int i = 0; i < 3; ++i) {
			const float x = cx + (firstSlot + i) * pitch;
			const float pairWidth = letterWidths[i] + 1.f + 2.f;
			centreAt(busInputs[i], Vec(x, kStripMm));
			anchorTextAt(inputLabels[i], Vec(x - pairWidth * 0.5f, labelY), false);
			centreAt(inputLights[i], Vec(x + pairWidth * 0.5f - 1.f, labelY));
		}
		const float selX = cx + (firstSlot + 3) * pitch;
		centreAt(selButton, Vec(selX, kStripMm));
		centreAt(selLabel, Vec(selX, labelY));
		const float thruX = cx + (firstSlot + 4) * pitch;
		thruPlate->box.pos = mm2px(Vec(thruX - PLATE_W_MM * 0.5f, kStripMm + PLATE_TOP_MM));
		centreAt(thruOut, Vec(thruX, kStripMm));

		// SHOT left, REC right. Wide panels put them on the strip row, four pitches out from the centre; the
		// narrower panels have no room there, so the pair sits on its own row above the strip.
		const float captureY = beside ? kStripMm : kCaptureAboveMm;
		const float captureDx = beside ? static_cast<float>(slots) * pitch : kCaptureAboveDxMm;
		const Vec shotCentre(cx - captureDx, captureY);
		const Vec recCentre(cx + captureDx, captureY);
		centreAt(shotButton, shotCentre);
		centreAt(shotLight, shotCentre);
		centreAt(recButton, recCentre);
		centreAt(recLight, recCentre);
		anchorTextAt(shotLabel, Vec(shotCentre.x - kCaptureLabelDxMm, captureY), true);
		anchorTextAt(recLabel, Vec(recCentre.x + kCaptureLabelDxMm, captureY), false);

		// L and R, the movie's audio. On the strip they follow THRU at the same pitch, with the letter centred
		// under the jack like SEL. At 12 HP they sit on a third row above SHOT and REC, letters on the outer sides.
		if (audioOnStrip) {
			for (int i = 0; i < 2; ++i) {
				const float x = cx + (firstSlot + 5 + i) * pitch;
				centreAt(audioPorts[i], Vec(x, kStripMm));
				centreAt(audioLabels[i], Vec(x, labelY));
			}
		}
		else {
			const float dx = kCaptureAboveDxMm;
			constexpr float jackRadiusMm = 4.1f;
			centreAt(audioPorts[0], Vec(cx - dx, kAudioAboveMm));
			centreAt(audioPorts[1], Vec(cx + dx, kAudioAboveMm));
			anchorTextAt(audioLabels[0], Vec(cx - dx - jackRadiusMm - LABEL_GAP_MM, kAudioAboveMm), true);
			anchorTextAt(audioLabels[1], Vec(cx + dx + jackRadiusMm + LABEL_GAP_MM, kAudioAboveMm), false);
		}
	}

	void step() override {
		ModuleWidget::step();
		if (displayModule) {
			// Runs for every Display every UI frame, drawn or not: button presses, worker results, lights.
			displayModule->uiTick();
			if (displayModule->panelHp != hp) {
				applyPanelHp(displayModule->panelHp);
			}
		}
	}

	void appendContextMenu(ui::Menu* menu) override {
		menu->addChild(new ui::MenuSeparator);
		if (displayModule) {
			Display* m = displayModule;
			// The folder picker lives here and nowhere else. The SHOT and REC buttons never open a dialog.
			menu->addChild(createMenuLabel("Screenshot and video folder"));
			menu->addChild(createMenuItem("Choose folder...", display_capture::shortenForMenu(m->effectiveFolder()),
				[m]() { m->chooseCaptureFolder(); }));
			menu->addChild(createMenuItem("Show folder", "",
				[m]() { m->showCaptureFolder(); },
				!display_capture::directoryExists(m->effectiveFolder())));
			menu->addChild(createMenuItem("Use default folder", display_capture::shortenForMenu(display_capture::defaultCaptureFolder()),
				[m]() { m->useDefaultFolder(); },
				m->captureFolder.empty()));
			if (!display_recorder::platformSupportsRecording()) {
				menu->addChild(createMenuLabel("Video recording needs macOS"));
			}
			menu->addChild(new ui::MenuSeparator);
		}
		if (displayModule) {
			Display* m = displayModule;
			menu->addChild(createSubmenuItem("Input", std::string(1, 'A' + m->selectedInput.load(std::memory_order_relaxed)), [m](ui::Menu* sub) {
				for (int i = 0; i < 3; ++i) {
					sub->addChild(createCheckMenuItem(std::string(1, 'A' + i), "",
						[m, i]() { return m->selectedInput.load(std::memory_order_relaxed) == i; },
						[m, i]() { m->selectedInput.store(i, std::memory_order_relaxed); }));
				}
			}));
		}
		menu->addChild(createSubmenuItem("Panel width", std::to_string(hp) + " HP", [this](ui::Menu* sub) {
			for (int choice : kHpChoices) {
				sub->addChild(createCheckMenuItem(std::to_string(choice) + " HP", "",
					[this, choice]() { return hp == choice; },
					[this, choice]() { applyPanelHp(choice); }));
			}
		}));
	}
};

Model* modelDisplay = createModel<Display, DisplayWidget>("Display");
