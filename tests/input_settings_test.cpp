#ifdef NDEBUG
#undef NDEBUG
#endif
#include "overlay/settings.h"
#include <cassert>
#include <filesystem>
#include <random>
int main() {
    namespace fs = std::filesystem;
    namespace o = nx::overlay;
    auto path = fs::temp_directory_path() / ("wonder-input-" + std::to_string(std::random_device{}()) + ".ini");
    o::Settings s;
    s.player_devices = {3, 2, 4, 1};
    s.keyboard_separate = true;
    s.button_icons = 3;
    s.pad_custom_mask[1] = 1u << o::CtlA;
    s.pad_bindings[1][o::CtlA] = o::PhyNorth;
    s.controller_custom_mask = 1u << o::CtlA;
    s.controller_bindings[o::CtlA] = o::PhyWest;
    s.hdr = 1, s.hdr_peak = 1200, s.hdr_paper_white = 250, s.hdr_highlights = 0, s.hdr_highlight_start = 175;
    o::save_settings(path.string(), s);
    o::Settings loaded;
    assert(o::load_settings(path.string(), loaded));
    assert(loaded.player_devices == s.player_devices && loaded.keyboard_separate);
    assert(loaded.button_icons == 3);
    assert(loaded.hdr == 1 && loaded.hdr_peak == 1200 && loaded.hdr_paper_white == 250);
    assert(loaded.hdr_highlights == 0 && loaded.hdr_highlight_start == 175);
    assert(o::controller_binding_for_slot(loaded, 0, o::CtlA) == o::PhyWest);
    assert(o::controller_binding_for_slot(loaded, 1, o::CtlA) == o::PhyNorth);
    assert(o::controller_binding_for_slot(loaded, 2, o::CtlA) == o::default_controller_binding(o::CtlA, s.controller_preset));
    fs::remove(path);
}
