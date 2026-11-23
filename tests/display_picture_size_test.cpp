// Headless Display "Picture size" check against Rack's real Module/JSON code. No GUI or running Rack.
// A new Display is 720 lines; its choice survives a save and load; a patch saved before the choice existed
// (no key) uses the last menu choice; the pixel sizes follow the 4:3 rule.
#include "../src/Display.cpp"
#include <cstdio>

Plugin* pluginInstance = nullptr;

int main() {
	int fails = 0, checks = 0;
	auto check = [&](bool v, const char* msg) { ++checks; if (!v) { ++fails; std::fprintf(stderr, "FAIL: %s\n", msg); } };

	rack::Context context;
	context.engine = new rack::engine::Engine;
	context.event = new rack::widget::EventState;
	rack::contextSet(&context);

	Plugin plugin;
	plugin.slug = "VideoResearchUnit";
	modelDisplay->plugin = &plugin;
	modelDisplay->slug = "Display";

	// A release from the opening double-click must leave the overlay up. A fresh click or a wake closes it.
	{
		Widget scene;
		auto* overlay = new FullscreenPicture;
		scene.addChild(overlay);
		Widget::ButtonEvent click{};
		click.button = GLFW_MOUSE_BUTTON_LEFT;
		click.action = GLFW_RELEASE;
		overlay->onButton(click);
		overlay->step();
		check(!overlay->requestedDelete, "opening release and regular frames keep the fullscreen picture");
		click.action = GLFW_PRESS;
		overlay->onButton(click);
		check(overlay->requestedDelete, "one click exits fullscreen");
		auto* sleeping = new FullscreenPicture;
		scene.addChild(sleeping);
		sleeping->lastStep -= std::chrono::seconds(11);
		sleeping->step();
		check(sleeping->requestedDelete, "first frame after suspension exits fullscreen");
	}

	// A Display added now is 720 lines.
	Display fresh;
	check(fresh.pictureLines == 720, "a new Display is 720 lines");

	displayPreferredLines.store(540);
	Display inherited;
	check(inherited.pictureLines == 540, "a new Display inherits the last menu choice");
	// Each choice survives a save and load.
	for (int lines : {1080, 720, 540}) {
		Display a;
		a.pictureLines = lines;
		json_t* saved = a.dataToJson();
		json_t* key = json_object_get(saved, "pictureLines");
		check(json_is_integer(key) && json_integer_value(key) == lines, "the choice is written to the patch");
		Display b;
		b.pictureLines = lines == 540 ? 1080 : 540;
		b.dataFromJson(saved);
		check(b.pictureLines == lines, "the choice loads back");
		json_decref(saved);
	}

	// Opening explicit patch sizes above did not change the remembered choice.
	{
		json_t* old = json_object();
		json_object_set_new(old, "panelHp", json_integer(24));
		json_object_set_new(old, "selectedInput", json_integer(1));
		Display d;
		check(d.pictureLines == 540, "patch loads leave the remembered choice alone");
		d.dataFromJson(old);
		check(d.pictureLines == 540, "a patch without the key loads as the remembered size");
		json_decref(old);
	}

	// A hand-edited or damaged value is not trusted: it becomes 1080 too.
	for (json_int_t bad : {json_int_t(0), json_int_t(-5), json_int_t(1000), json_int_t(4320)}) {
		json_t* j = json_object();
		json_object_set_new(j, "pictureLines", json_integer(bad));
		Display d;
		d.dataFromJson(j);
		check(d.pictureLines == 540, "an unlisted value loads as the remembered size");
		json_decref(j);
	}
	{
		json_t* j = json_object();
		json_object_set_new(j, "pictureLines", json_string("720"));
		Display d;
		d.dataFromJson(j);
		check(d.pictureLines == 540, "a value of the wrong kind loads as the remembered size");
		json_decref(j);
	}

	// The sizes the menu shows and the picture uses (4:3).
	int w = 0, h = 0;
	displayChoiceSize(1080, w, h);
	check(w == 1440 && h == 1080, "the remembered size is 1440x1080");
	displayChoiceSize(720, w, h);
	check(w == 960 && h == 720, "720 lines is 960x720");
	displayChoiceSize(540, w, h);
	check(w == 720 && h == 540, "540 lines is 720x540");

	// A big screen is capped by the choice; a small one is not enlarged; the floor is 480.
	displayPictureSize(1700.f, 720, w, h);
	check(w == 960 && h == 720, "a tall screen is capped at 720 lines");
	displayPictureSize(600.f, 720, w, h);
	check(h == 600 && w == 800, "a screen under the cap keeps its own height");
	displayPictureSize(200.f, 540, w, h);
	check(h == 480 && w == 640, "the floor is 480 lines");
	displayPictureSize(1700.f, 1080, w, h);
	check(w == 1440 && h == 1080, "at 1080 the cap is the old one");

	std::printf("display_picture_size_test: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
