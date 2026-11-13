#pragma once

#include "plugin.hpp"

#include <string>
#include <vector>

// Shared panel vocabulary for Video Research Unit (VRU) modules, modelled on Grayscale and
// Synthesis Technology panels:
//
// - Panel artwork is a flat light-grey SVG in res/ (background and at most a
//   hairline rule under the header). Rack's SVG loader cannot draw text, so all
//   labels are drawn here with NanoVG.
// - Width is whole HP (5.08 mm). Layout uses a 10.16 mm grid: rows of controls
//   are centred on the panel width, GRID_MM apart. A row of three or four
//   controls may use a wider pitch (rowX takes one) so that labels do not touch.
// - Header: the series line VIDEO RESEARCH UNIT over a short hairline, then the catalogue number
//   (VRU–01) and the bold uppercase module name, all above the panel's own rule at y = 15.4. Display is the
//   one exception: a single line, so its picture keeps its full size.
// - Knobs: medium for main controls, small for secondary ones. Labels sit above.
// - Inputs: plain jack, label above.
// - Outputs: jack on a black rounded plate with a reversed label, grouped at the bottom.
// - A module's main signal inputs may sit in an outlined plate, label inside; filled plates stay
//   outputs only. The outlined plate is the output plate's size, unfilled, so it reads as an input
//   and a row of inputs and outputs lines up (addPlatedInput).
//
// Only layout and drawing change. Param, input, output and light IDs stay as
// they are, so saved patches keep loading.

namespace rasterlab_panel {

constexpr float HP_MM = 5.08f;
constexpr float GRID_MM = 10.16f;

// Text sizes in mm. These are NanoVG font sizes, which for Nunito-Bold is the
// line height (1.255 em), so a capital is 0.561 x the size: 4.9 gives a 2.75 mm
// cap, 3.8 gives 2.13 mm, 3.4 gives 1.91 mm, 3.2 gives 1.80 mm, 2.8 gives 1.57 mm. Control labels
// are about 1.8 x their old size (2.1, cap 1.18 mm) so that they read like the
// labels on VCV Fundamental's Scope (cap about 2.0 mm); the title and the two
// smaller header lines scale with them so the ratios between the three hold.
constexpr float TITLE_MM = 4.9f;        // module name
constexpr float NUMBER_MM = 3.4f;       // catalogue number line (VRU–01)
constexpr float SERIES_MM = 2.8f;       // VIDEO RESEARCH UNIT line
constexpr float LABEL_MM = 3.8f;        // control labels
constexpr float NOTE_MM = 3.2f;         // words beside a toggle's lever ends
constexpr float PLATE_TEXT_MM = 3.4f;   // reversed text on an output plate
constexpr float READOUT_MM = 4.7f;      // headline line of a status readout (detail lines use LABEL_MM)
constexpr float CAP_RATIO = 0.561f;

// Header band: centre y of each element (mm). The band ends above the panel's own hairline rule, which the
// SVG draws at y = 15.4.
constexpr float SERIES_Y_MM = 3.8f;        // VIDEO RESEARCH UNIT
constexpr float SERIES_RULE_Y_MM = 6.0f;   // hairline under the series line
constexpr float NUMBER_Y_MM = 8.45f;       // VRU–01
constexpr float TITLE_Y_MM = 12.3f;        // module name

// Half height of the controls a label sits above (mm), so a label can be
// placed from the control's edge, not from a fixed offset.
constexpr float R_KNOB = 4.8f;      // RoundBlackKnob, RoundBlackSnapKnob
constexpr float R_SMALL = 3.84f;    // RoundSmallBlackKnob
constexpr float R_TRIM = 3.03f;     // Trimpot
constexpr float R_SWITCH = 3.5f;    // CKSS (6.99 mm tall)
constexpr float R_JACK = 4.0f;      // PJ301MPort
constexpr float R_BUTTON = 3.05f;   // LEDButton, VCVButton

// Clear space between a control's edge and the bottom of its label's capitals,
// and the line pitch of a two-line label (text containing '\n').
constexpr float LABEL_GAP_MM = 1.4f;
constexpr float LINE_PITCH_MM = 3.4f;

// Position words beside a toggle: distance from the lever column and from the
// toggle centre to the upper and lower lever ends.
constexpr float TOGGLE_WORD_DX_MM = 3.8f;
constexpr float TOGGLE_WORD_DY_MM = 2.5f;

// Grid pitch for a 3-column layout inside an 8 HP panel (40.64 mm).
constexpr float COL_1_MM = 10.16f;
constexpr float COL_2_MM = 20.32f;
constexpr float COL_3_MM = 30.48f;

// Plate behind outputs (and, outlined, behind a module's main signal inputs): width, height, and
// offset of the label centre from the jack centre (all in mm).
constexpr float PLATE_W_MM = 9.2f;
constexpr float PLATE_H_MM = 11.8f;
constexpr float PLATE_TOP_MM = -4.2f;   // plate top relative to jack centre
constexpr float PLATE_LABEL_MM = 5.6f;  // label centre relative to jack centre
constexpr float PLATE_RADIUS_MM = 1.2f;
constexpr float PLATE_OUTLINE_MM = 0.35f;  // line width of an input plate's outline

// Centre x of item k of n in a row, pitchMm apart (GRID_MM by default), centred in widthMm.
inline float rowX(float widthMm, int n, int k, float pitchMm = GRID_MM) {
	return widthMm * 0.5f + (k - (n - 1) * 0.5f) * pitchMm;
}

// Distance from a control's centre up to the centre of its label.
inline float labelOffset(float controlRadiusMm) {
	return controlRadiusMm + LABEL_GAP_MM + CAP_RATIO * LABEL_MM * 0.5f;
}

// Selects Rack's Nunito-Bold on a NanoVG context. Every size in this file is measured for that face,
// so panel text and the status readouts all call this before they draw (NanoVG's default face is not
// Nunito, and its metrics differ).
inline void useLabelFont(NVGcontext* vg) {
	auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
	if (font)
		nvgFontFaceId(vg, font->handle);
}

// Shortens text with "..." until it fits maxWidth (px) at the current NanoVG font size. Cuts on UTF-8
// character boundaries, so a multi-byte character (an en dash in a server name) is never split. Status
// readouts use it so a long name never runs past the edge of its screen.
inline std::string fitToWidth(NVGcontext* vg, const std::string& text, float maxWidth) {
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

// Text drawn at an anchor point. sizeMm is the NanoVG font size in mm (see above).
// hAlign is NVG_ALIGN_CENTER (anchor is the middle of the text), NVG_ALIGN_LEFT
// (anchor is the left edge) or NVG_ALIGN_RIGHT.
struct PanelText : TransparentWidget {
	std::string text;
	float sizeMm = 2.2f;
	NVGcolor color = nvgRGB(27, 27, 27);
	int hAlign = NVG_ALIGN_CENTER;

	void draw(const DrawArgs& args) override {
		useLabelFont(args.vg);
		nvgFontSize(args.vg, mm2px(sizeMm));
		nvgTextAlign(args.vg, hAlign | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, color);
		float x = box.size.x * 0.5f;
		if (hAlign == NVG_ALIGN_LEFT)
			x = 0.f;
		else if (hAlign == NVG_ALIGN_RIGHT)
			x = box.size.x;
		nvgText(args.vg, x, box.size.y * 0.5f, text.c_str(), nullptr);
	}
};

inline PanelText* createPanelText(Vec centerMm, const std::string& text, float sizeMm, NVGcolor color = nvgRGB(27, 27, 27)) {
	auto* label = new PanelText();
	label->text = text;
	label->sizeMm = sizeMm;
	label->color = color;
	const Vec size = mm2px(Vec(30.f, sizeMm * 2.f));
	const Vec center = mm2px(centerMm);
	label->box.size = size;
	label->box.pos = Vec(center.x - size.x * 0.5f, center.y - size.y * 0.5f);
	return label;
}

// Text whose left (NVG_ALIGN_LEFT) or right (NVG_ALIGN_RIGHT) edge is at anchorMm.x
// and whose vertical centre is anchorMm.y.
inline PanelText* createPanelTextAt(Vec anchorMm, const std::string& text, float sizeMm, int hAlign, NVGcolor color = nvgRGB(27, 27, 27)) {
	auto* label = new PanelText();
	label->text = text;
	label->sizeMm = sizeMm;
	label->color = color;
	label->hAlign = hAlign;
	const Vec size = mm2px(Vec(30.f, sizeMm * 2.f));
	const Vec anchor = mm2px(anchorMm);
	label->box.size = size;
	label->box.pos = Vec(hAlign == NVG_ALIGN_RIGHT ? anchor.x - size.x : anchor.x, anchor.y - size.y * 0.5f);
	return label;
}

// A horizontal hairline. The widget's box spans the line from side to side and is a little taller than
// the stroke; the line runs down the middle of the box. strokeMm is the stroke width in mm (the SVG
// panels' own rule is 0.15 mm).
struct PanelRule : TransparentWidget {
	float strokeMm = 0.15f;
	NVGcolor color = nvgRGB(27, 27, 27);

	// Puts the line from x1Mm to x2Mm at y = yMm (panel mm), the same way createPanelText places text by its
	// centre.
	void place(float x1Mm, float x2Mm, float yMm) {
		const Vec size = mm2px(Vec(x2Mm - x1Mm, 1.f));
		box.size = size;
		box.pos = Vec(mm2px(x1Mm), mm2px(yMm) - size.y * 0.5f);
	}

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 0.f, box.size.y * 0.5f);
		nvgLineTo(args.vg, box.size.x, box.size.y * 0.5f);
		nvgStrokeColor(args.vg, color);
		nvgStrokeWidth(args.vg, mm2px(strokeMm));
		nvgStroke(args.vg);
	}
};

inline PanelRule* createPanelRule(float x1Mm, float x2Mm, float yMm, float strokeMm = 0.15f, NVGcolor color = nvgRGB(27, 27, 27)) {
	auto* rule = new PanelRule();
	rule->strokeMm = strokeMm;
	rule->color = color;
	rule->place(x1Mm, x2Mm, yMm);
	return rule;
}

// Black rounded plate behind an output jack, with the label reversed inside.
// Add this before the output port so the jack draws on top of it.
struct OutputPlate : TransparentWidget {
	std::string text;
	float sizeMm = PLATE_TEXT_MM;

	void draw(const DrawArgs& args) override {
		useLabelFont(args.vg);

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, mm2px(PLATE_RADIUS_MM));
		nvgFillColor(args.vg, nvgRGB(17, 17, 17));
		nvgFill(args.vg);

		nvgFontSize(args.vg, mm2px(sizeMm));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, nvgRGB(242, 242, 240));
		nvgText(args.vg, box.size.x * 0.5f, mm2px(PLATE_LABEL_MM - PLATE_TOP_MM), text.c_str(), nullptr);
	}
};

// jackCenterMm is the same point passed to addOutput()/createOutputCentered(). plateWidthMm is the
// plate width: the standard 9.2 mm fits labels up to about four letters; a longer word asks for more.
inline OutputPlate* createOutputPlate(Vec jackCenterMm, const std::string& text, float sizeMm = PLATE_TEXT_MM, float plateWidthMm = PLATE_W_MM) {
	auto* plate = new OutputPlate();
	plate->text = text;
	plate->sizeMm = sizeMm;
	const Vec size = mm2px(Vec(plateWidthMm, PLATE_H_MM));
	const Vec topLeft = mm2px(Vec(jackCenterMm.x - plateWidthMm * 0.5f, jackCenterMm.y + PLATE_TOP_MM));
	plate->box.pos = topLeft;
	plate->box.size = size;
	return plate;
}

// Outlined plate behind a module's main signal input: the output plate's footprint, not filled, with a
// dark outline and the label in dark text inside, below the jack. Filled plates stay outputs only, so
// the outline is what sets an input plate apart. Add this before the input port so the jack draws on
// top of it.
struct InputPlate : TransparentWidget {
	std::string text;
	float sizeMm = PLATE_TEXT_MM;

	void draw(const DrawArgs& args) override {
		useLabelFont(args.vg);

		// A stroke is centred on its path, so the path is inset by half the line width: the outline's
		// outer edge then lands where an OutputPlate's edge does, and both occupy the same 9.2 x 11.8 mm.
		const float line = mm2px(PLATE_OUTLINE_MM);
		const float inset = line * 0.5f;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, inset, inset, box.size.x - line, box.size.y - line, mm2px(PLATE_RADIUS_MM) - inset);
		nvgStrokeColor(args.vg, nvgRGB(27, 27, 27));
		nvgStrokeWidth(args.vg, line);
		nvgStroke(args.vg);

		nvgFontSize(args.vg, mm2px(sizeMm));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, nvgRGB(27, 27, 27));
		nvgText(args.vg, box.size.x * 0.5f, mm2px(PLATE_LABEL_MM - PLATE_TOP_MM), text.c_str(), nullptr);
	}
};

// jackCenterMm is the same point passed to addInput()/createInputCentered(). plateWidthMm is the
// plate width, as for createOutputPlate().
inline InputPlate* createInputPlate(Vec jackCenterMm, const std::string& text, float sizeMm = PLATE_TEXT_MM, float plateWidthMm = PLATE_W_MM) {
	auto* plate = new InputPlate();
	plate->text = text;
	plate->sizeMm = sizeMm;
	const Vec size = mm2px(Vec(plateWidthMm, PLATE_H_MM));
	const Vec topLeft = mm2px(Vec(jackCenterMm.x - plateWidthMm * 0.5f, jackCenterMm.y + PLATE_TOP_MM));
	plate->box.pos = topLeft;
	plate->box.size = size;
	return plate;
}

// The catalogue number line: "VRU–" and the two digits ("01" gives "VRU–01"). The dash is an en dash
// (U+2013), written as its three UTF-8 bytes and joined to the digits at run time: a hex escape takes
// every hex digit that follows it, so "VRU\xe2\x80\x9301" would swallow the 0 and the 1.
inline std::string catalogueNumber(const std::string& number) {
	return std::string("VRU\xe2\x80\x93") + number;
}

// The series line above every header.
constexpr const char* SERIES_TEXT = "VIDEO RESEARCH UNIT";

// The header band, centred on the panel and all above the SVG's own rule at y = 15.4: the series line with a
// short hairline under it, the catalogue number (number is the two digits, "01"), and the module name in
// bold capitals. ruleInsetMm is the inset of the panel's own rule from each side (4 mm, or 3 mm on Deflection
// Yoke); the series rule spans the same x range, so the two rules line up. A title that is too wide for the
// rule (DUAL LFO CHAOS) passes a smaller titleMm. Display does not use this band: it keeps its whole picture
// and builds its own one-line header (see the layout comment in Display.cpp).
inline void addPanelHeader(ModuleWidget* w, float widthMm, const std::string& number, const std::string& title, float titleMm = TITLE_MM, float ruleInsetMm = 4.f) {
	const float centreX = widthMm * 0.5f;
	w->addChild(createPanelText(Vec(centreX, SERIES_Y_MM), SERIES_TEXT, SERIES_MM, nvgRGB(90, 90, 88)));
	w->addChild(createPanelRule(ruleInsetMm, widthMm - ruleInsetMm, SERIES_RULE_Y_MM));
	w->addChild(createPanelText(Vec(centreX, NUMBER_Y_MM), catalogueNumber(number), NUMBER_MM));
	w->addChild(createPanelText(Vec(centreX, TITLE_Y_MM), title, titleMm));
}

// A label made of one or more lines (separated by '\n'), the last line centred on
// bottomCentreMm and earlier lines stacked above it, LINE_PITCH_MM apart.
inline void addLabelLines(ModuleWidget* w, Vec bottomCentreMm, const std::string& text, float sizeMm) {
	std::vector<std::string> lines;
	size_t start = 0;
	while (true) {
		const size_t nl = text.find('\n', start);
		if (nl == std::string::npos) {
			lines.push_back(text.substr(start));
			break;
		}
		lines.push_back(text.substr(start, nl - start));
		start = nl + 1;
	}
	for (size_t i = 0; i < lines.size(); i++) {
		const float y = bottomCentreMm.y - (float) (lines.size() - 1 - i) * LINE_PITCH_MM;
		w->addChild(createPanelText(Vec(bottomCentreMm.x, y), lines[i], sizeMm));
	}
}

// Label above a knob, switch or light whose centre is centreMm. radiusMm is half
// the control's height (R_KNOB, R_SMALL, R_TRIM, R_SWITCH, R_BUTTON). A text with
// '\n' becomes a stack of lines, the last one nearest the control.
inline void addKnobLabel(ModuleWidget* w, Vec centreMm, const std::string& text, float radiusMm = R_KNOB) {
	addLabelLines(w, Vec(centreMm.x, centreMm.y - labelOffset(radiusMm)), text, LABEL_MM);
}

// Label above a jack whose centre is centreMm.
inline void addJackLabel(ModuleWidget* w, Vec centreMm, const std::string& text, float radiusMm = R_JACK) {
	addLabelLines(w, Vec(centreMm.x, centreMm.y - labelOffset(radiusMm)), text, LABEL_MM);
}

// Name and position words for a two-position toggle (CKSS). For a CKSS value 1 is the lever up and
// value 0 is down, so upWord names value 1 and downWord names value 0.
//   side +1: words to the right of the toggle, starting after the lever column.
//   side -1: words to the left of the toggle, ending before the lever column.
//   Both put the name above the toggle at the label line for labelRadiusMm (pass R_KNOB to line it up
//   with big-knob labels in the same row).
inline void addToggleLabels(ModuleWidget* w, Vec centreMm, const std::string& name, const std::string& upWord, const std::string& downWord, int side = 1, float labelRadiusMm = R_SWITCH) {
	addLabelLines(w, Vec(centreMm.x, centreMm.y - labelOffset(labelRadiusMm)), name, LABEL_MM);
	const int align = side < 0 ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT;
	const float x = centreMm.x + (side < 0 ? -TOGGLE_WORD_DX_MM : TOGGLE_WORD_DX_MM);
	w->addChild(createPanelTextAt(Vec(x, centreMm.y - TOGGLE_WORD_DY_MM), upWord, NOTE_MM, align));
	w->addChild(createPanelTextAt(Vec(x, centreMm.y + TOGGLE_WORD_DY_MM), downWord, NOTE_MM, align));
}

// Output jack on its black plate, with the plate label. plateWidthMm as for createOutputPlate().
template <typename TPort>
inline void addPlatedOutput(ModuleWidget* w, Vec centreMm, const std::string& text, engine::Module* module, int outputId, float plateWidthMm = PLATE_W_MM) {
	w->addChild(createOutputPlate(centreMm, text, PLATE_TEXT_MM, plateWidthMm));
	w->addOutput(createOutputCentered<TPort>(mm2px(centreMm), module, outputId));
}

// Input jack in its outlined plate, with the plate label. plateWidthMm as for createInputPlate().
template <typename TPort>
inline void addPlatedInput(ModuleWidget* w, Vec centreMm, const std::string& text, engine::Module* module, int inputId, float plateWidthMm = PLATE_W_MM) {
	w->addChild(createInputPlate(centreMm, text, PLATE_TEXT_MM, plateWidthMm));
	w->addInput(createInputCentered<TPort>(mm2px(centreMm), module, inputId));
}

} // namespace rasterlab_panel
