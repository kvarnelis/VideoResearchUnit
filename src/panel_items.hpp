#pragma once
#include "plugin.hpp"
#include "panel_style.hpp"

#include <algorithm>
#include <string>

// Places the items of a panel layout table (Video Probe's src/video_probe_layout.hpp, Raster's src/raster_layout.hpp).
// scripts/check-panel.py reads the same table to draw the panel to scale and to fail on any overlap, so the code and the
// check cannot disagree about where a control is. X(kind, x, y, "label", id, extra), mm from the panel's top left:
//   KNOB, SNAP, SMALL, TRIM   black knob, stepped black knob, small knob, trimmer; label above
//   SW2      two-position switch: "NAME|UP|DOWN", the name above, the word for lever up and for lever down beside it
//   JACK     input jack, label above
//   OUT      output jack on its dark plate, extra is the plate width; label in the plate
//   LIGHT    green light, label above
//   TEXT     text centred at x, y (extra is the size in mm); LEFT is the same with its left edge at x
//   RULE     hairline from x to extra at y
namespace panel_items {

// Plate label size: 3.4 mm at most, and no more than 26% of the plate's width, the same rule for every plate of a panel so the
// words match (scripts/check-panel.py uses the same rule and fails if a word is not inside its plate with 0.5 mm to spare).
inline float plateTextMm(float plateWidthMm) { return std::min(rasterlab_panel::PLATE_TEXT_MM, 0.26f * plateWidthMm); }

enum class Kind { KNOB, SNAP, SMALL, TRIM, SW2, JACK, OUT, LIGHT, TEXT, LEFT, RULE };

template <class TKnob>
inline void knob(ModuleWidget* w, Module* m, float x, float y, const char* label, int id, float radiusMm) {
	if (label[0]) rasterlab_panel::addKnobLabel(w, Vec(x, y), label, radiusMm);
	w->addParam(createParamCentered<TKnob>(mm2px(Vec(x, y)), m, id));
}

inline void place(ModuleWidget* w, Module* m, Kind kind, float x, float y, const char* label, int id, float extra) {
	using namespace rasterlab_panel;
	switch (kind) {
	case Kind::KNOB: knob<RoundBlackKnob>(w, m, x, y, label, id, R_KNOB); break;
	case Kind::SNAP: knob<RoundBlackSnapKnob>(w, m, x, y, label, id, R_KNOB); break;
	case Kind::SMALL: knob<RoundSmallBlackKnob>(w, m, x, y, label, id, R_SMALL); break;
	case Kind::TRIM: knob<Trimpot>(w, m, x, y, label, id, R_TRIM); break;
	case Kind::SW2: {
		const std::string text = label;
		const size_t a = text.find('|'), b = text.find('|', a + 1);
		addToggleLabels(w, Vec(x, y), text.substr(0, a), text.substr(a + 1, b - a - 1), text.substr(b + 1));
		w->addParam(createParamCentered<CKSS>(mm2px(Vec(x, y)), m, id));
		break;
	}
	case Kind::JACK:
		if (label[0]) addJackLabel(w, Vec(x, y), label);
		w->addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, y)), m, id));
		break;
	case Kind::OUT: {
		w->addChild(createOutputPlate(Vec(x, y), label, plateTextMm(extra), extra));
		w->addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, y)), m, id));
		break;
	}
	case Kind::LIGHT:
		addJackLabel(w, Vec(x, y), label);
		w->addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(x, y)), m, id));
		break;
	case Kind::TEXT: w->addChild(createPanelText(Vec(x, y), label, extra)); break;
	case Kind::LEFT: w->addChild(createPanelTextAt(Vec(x, y), label, extra, NVG_ALIGN_LEFT)); break;
	case Kind::RULE: w->addChild(createPanelRule(x, extra, y)); break;
	}
}

}  // namespace panel_items

// Use inside a ModuleWidget constructor that has a `module` pointer: PANEL_PLACE_ALL(MY_LAYOUT)
#define PANEL_PLACE_ITEM(kind, x, y, label, id, extra) panel_items::place(this, module, panel_items::Kind::kind, x, y, label, id, extra);
#define PANEL_PLACE_ALL(LAYOUT) LAYOUT(PANEL_PLACE_ITEM)
