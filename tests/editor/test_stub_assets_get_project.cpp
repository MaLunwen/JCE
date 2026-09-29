// test_stub_assets_get_project.cpp — minimal stub so the editor TU links.
//
// The production implementation lives in editor/src/core/jce_assetdb.cpp,
// which pulls in cjson, bgfx, SDL_image, and the rest of the editor world.
// For the path-util unit test we only need a symbol that returns a stable
// project root, so we provide a trivial one here.

#include <cstddef>

extern "C" const char *jce_editor_assets_get_project(void)
{
    return "";
}
