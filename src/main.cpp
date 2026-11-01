// TEST BUILD ONLY (ios-test-nohooks branch): same mod id, settings and resources as Click
// Before Frames, but no hooks and no mod code. Used to tell whether the iOS (JIT-less)
// launcher crash on the loading screen comes from the mod's hooks or from the mod package.
#include <Geode/Geode.hpp>

using namespace geode::prelude;

$on_mod(Loaded) {
    geode::log::info("startup step: empty test build loaded (no hooks)");
}
