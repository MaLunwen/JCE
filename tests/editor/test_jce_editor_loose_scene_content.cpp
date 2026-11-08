#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_project_context.h"
#include "scene/jce_scene_content_context.h"
#include "jce_test_owned_directory.h"
#include <jce/application/jce_project.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_render_settings.h>
#include <cstdio>
#include <cstring>
#include <string>

/* Real editor path decomposition and real project/host filesystem owners.
 * Only the subsystem publication destinations are counted typed ports. */
extern "C" const char *jce_editor_path_basename_view(const char *path);
extern "C" void jce_editor_path_trim_to_parent(char *path);
static std::string asset_browser_root;
extern "C" const char *jce_editor_assets_get_project(void)
{
    return asset_browser_root.c_str();
}

namespace actual {
char s_current_project_root[512]{};
static JceProject *project = nullptr;
static std::string model_root, particle_root, canvas_root;
static std::string default_font, fallback_fonts;
static unsigned transitions, module_reloads;
static unsigned publications, bundle_retirements;
static bool switch_allowed = true;
static bool publication_allowed = true;
static JceFileSystem *active_fs = nullptr;
static JceFsActivePolicy active_policy = JCE_FS_ACTIVE_OVERLAY;
struct FileEntry { bool is_dir = false; std::string path; };
static unsigned scene_loads, file_opens, directory_opens;
static bool scene_load_allowed = true, asset_is_scene = true;
constexpr int JCE_ASSET_KIND_SCENE = 1;
int jce_assetdb_get_kind(const char *) { return asset_is_scene ? JCE_ASSET_KIND_SCENE : 0; }
bool jce_state_load_scene_file(const char *path);
void open_asset_in_file_viewer(const char *) { ++file_opens; }
void navigate_asset_directory(const std::string &, bool) { ++directory_opens; }
struct { void *scene_renderer = nullptr; } s_sr;
constexpr int JCE_LOG_LEVEL_INFO = 1;
constexpr float JCE_PARTICLES_SOFT_FADE_DEFAULT = 0.5f;
const char *jce_editor_current_project_root() { return s_current_project_root; }
const JceProject *jce_editor_project_get() { return project; }
bool jce_editor_prepare_project_switch(const char *next)
{
    if (jce_editor_project_path_equal(s_current_project_root, next)) return true;
    ++publications;
    return publication_allowed;
}
void jce_editor_assets_set_project(const char *value) { asset_browser_root = value; }
bool set_current_project_root(const char *value)
{
    ++transitions;
    if (!switch_allowed) return false;
    JceProject *next = value && value[0] ? jce_project_load(value) : nullptr;
    if (value && value[0] && !next && !jce_fs_host_exists_dir(value)) return false;
    if (active_fs && !jce_editor_project_path_equal(s_current_project_root, value)) {
        ++bundle_retirements;
        active_fs = nullptr;
        active_policy = JCE_FS_ACTIVE_OVERLAY;
    }
    jce_project_free(project);
    project = next;
    std::snprintf(s_current_project_root, sizeof(s_current_project_root), "%s",
                  next ? next->project_root : (value ? value : ""));
    return true;
}
void jce_editor_script_modules_reload(const char *) { ++module_reloads; }
JceFileSystem *jce_fs_get_active() { return active_fs; }
JceFsActivePolicy jce_fs_get_active_policy() { return active_policy; }
void jce_scene_particles_set_asset_root(const char *value)
{
    particle_root = value ? value : "";
}
void jce_ui_canvas_set_asset_root(const char *value)
{
    canvas_root = value ? value : "";
}
void jce_ui_canvas_set_default_font(const char *value)
{
    default_font = value ? value : "";
}
void jce_ui_canvas_set_font_fallbacks(const char *value)
{
    fallback_fonts = value ? value : "";
}
void jce_editor_scene_asset_cache_set_scene_dir(const char *value)
{
    model_root = value ? value : "";
}
JceRenderSettings jce_render_settings_default() { return {}; }
bool jce_editor_effective_render_settings(bool, const char *, JceRenderSettings *,
    std::string *, const char *, const char *, const char * = nullptr) { return false; }
void jce_scene_renderer_set_grass_enabled(void *, bool) {}
void jce_lod_set_global_bias(float) {}
float jce_lod_get_global_bias() { return 1.0f; }
void jce_particles_set_soft_fade_distance(float) {}
float jce_particles_get_soft_fade_distance() { return 0.0f; }
template<class... Args> void jce_log_write(Args...) {}
#define LOG_INFO(...) ((void)0)
#include JCE_TEST_EDITOR_LOOSE_CONTENT_BODY
#undef LOG_INFO
bool jce_state_load_scene_file(const char *path)
{
    ++scene_loads;
    return scene_load_allowed && follow_scene_project_root(path);
}
}

struct Fixture {
    JceTestOwnedDirectory owned{"_ut_loose_scene_content"};
    Fixture()
    {
        actual::switch_allowed = true;
        actual::publication_allowed = true;
        REQUIRE(actual::set_current_project_root(nullptr));
        actual::transitions = actual::module_reloads = 0;
        actual::publications = actual::bundle_retirements = 0;
        actual::scene_loads = actual::file_opens = actual::directory_opens = 0;
        actual::scene_load_allowed = actual::asset_is_scene = true;
        actual::active_fs = nullptr;
        actual::active_policy = JCE_FS_ACTIVE_OVERLAY;
        actual::jce_editor_scene_set_scene_dir("");
        actual::jce_editor_scene_render_refresh_content_context();
    }
    ~Fixture() { actual::switch_allowed = true; actual::set_current_project_root(nullptr); }
    std::string project(const char *name)
    {
        const std::string root = owned.root + "/" + name;
        REQUIRE(jce_fs_host_create_directory(root.c_str()));
        const char manifest[] = "{\"schema\":3,\"name\":\"fixture\","
            "\"source_assets\":\"content/source\",\"cooked_assets\":\"content/cooked\"}";
        REQUIRE(jce_fs_host_write_all((root + "/jce_project.json").c_str(),
                                     manifest, sizeof(manifest) - 1));
        return root;
    }
    std::string loose(const char *name, const char *directory = "scenes")
    {
        const std::string root = owned.root + "/" + name;
        REQUIRE(jce_fs_host_create_directory((root + "/" + directory).c_str()));
        REQUIRE(jce_fs_host_create_directory((root + "/fonts").c_str()));
        const char data[] = "owned content witness";
        REQUIRE(jce_fs_host_write_all((root + "/fonts/face.ttf").c_str(),
                                     data, sizeof(data) - 1));
        return root;
    }
};

TEST_CASE("loose scene binds the shared content root before deserialization")
{
    Fixture f;
    const auto root = f.loose("standalone");
    REQUIRE(actual::follow_scene_project_root((root + "/scenes/main.scene.json").c_str()));
    CHECK(actual::model_root == root);
    CHECK(actual::particle_root == root);
    CHECK(actual::canvas_root == root);
    CHECK(actual::s_current_project_root[0] == '\0');
    CHECK_FALSE(actual::project);
    const auto font = actual::canvas_root + "/fonts/face.ttf";
    CHECK(jce_fs_host_exists_file(font.c_str()));
    actual::consume_game_content_context();
    CHECK(actual::canvas_root == root);
    CHECK(actual::particle_root == root);
    actual::consume_game_content_context();
    CHECK(actual::canvas_root == root);
}

TEST_CASE("loose scene retires stale project through its existing transaction")
{
    Fixture f;
    const auto old = f.project("old"), root = f.loose("next", "Scenes");
    REQUIRE(actual::set_current_project_root(old.c_str()));
    actual::jce_editor_scene_render_refresh_content_context();
    actual::transitions = 0;
    REQUIRE(actual::follow_scene_project_root((root + "/Scenes/main.scene.json").c_str()));
    CHECK(actual::transitions == 1);
    CHECK(actual::s_current_project_root[0] == '\0');
    CHECK_FALSE(actual::project);
    CHECK(actual::canvas_root == root);
    CHECK(actual::particle_root == root);
    CHECK(actual::default_font.empty());
    CHECK(actual::fallback_fonts.empty());
}

TEST_CASE("refused loose transition preserves prior content ownership")
{
    Fixture f;
    const auto old = f.project("owner"), root = f.loose("refused");
    REQUIRE(actual::set_current_project_root(old.c_str()));
    actual::jce_editor_scene_set_scene_dir("old-model-root");
    actual::jce_editor_scene_render_refresh_content_context();
    const auto previous_canvas = actual::canvas_root;
    actual::switch_allowed = false;
    CHECK_FALSE(actual::follow_scene_project_root((root + "/scenes/main.scene.json").c_str()));
    CHECK(std::string(actual::s_current_project_root) == old);
    CHECK(actual::canvas_root == previous_canvas);
    CHECK(actual::model_root == "old-model-root");
}

TEST_CASE("manifest root wins and Bundle preserves valid authoring context")
{
    Fixture f;
    const auto root = f.project("authoring"), loose = f.loose("archive");
    REQUIRE(actual::set_current_project_root(root.c_str()));
    REQUIRE(actual::follow_scene_project_root((loose + "/content.jbundle").c_str(), true));
    CHECK(std::string(actual::s_current_project_root) == root);
    CHECK(actual::canvas_root == root + "/content/source");
    actual::active_fs = reinterpret_cast<JceFileSystem *>(uintptr_t{1});
    actual::active_policy = JCE_FS_ACTIVE_ISOLATED;
    actual::jce_editor_scene_render_refresh_content_context();
    CHECK(actual::canvas_root.empty());
    CHECK(actual::particle_root.empty());
    actual::consume_game_content_context();
    CHECK(actual::canvas_root.empty());
    CHECK(actual::particle_root.empty());
}

TEST_CASE("loose preflight refuses outgoing publication before a destructive transition")
{
    Fixture f;
    const auto old = f.project("outgoing"), loose = f.loose("incoming");
    REQUIRE(actual::set_current_project_root(old.c_str()));
    actual::jce_editor_scene_set_scene_dir("prior-model-root");
    const auto previous_canvas = actual::canvas_root;
    actual::publication_allowed = false;
    CHECK_FALSE(actual::prepare_scene_project_switch((loose + "/scenes/main.scene.json").c_str()));
    CHECK(actual::publications == 1);
    CHECK(std::string(actual::s_current_project_root) == old);
    CHECK(actual::model_root == "prior-model-root");
    CHECK(actual::canvas_root == previous_canvas);
    actual::publication_allowed = true;
    CHECK(actual::prepare_scene_project_switch((loose + "/scenes/main.scene.json").c_str()));
    CHECK(actual::publications == 2);
    CHECK(std::string(actual::s_current_project_root) == old);
}

TEST_CASE("virtual scene addresses never become host content roots")
{
    Fixture f;
    const auto root = f.loose("source");
    REQUIRE(actual::follow_scene_project_root((root + "/scenes/main.scene.json").c_str()));
    actual::update_scene_dir_from_path("bundle://package.jbundle!scenes/main.scene.json");
    CHECK(std::string(actual::jce_editor_scene_content_root()).empty());
    actual::update_scene_dir_from_path("catalog://catalog.json#scenes/main.scene.json");
    CHECK(std::string(actual::jce_editor_scene_content_root()).empty());
}

TEST_CASE("manifest and successive loose scenes replace one content owner")
{
    Fixture f;
    const auto a = f.loose("first"), b = f.loose("second");
    const auto project = f.project("manifest");
    REQUIRE(actual::follow_scene_project_root((a + "/scenes/main.scene.json").c_str()));
    CHECK(asset_browser_root == a);
    REQUIRE(actual::follow_scene_project_root((project + "/content/source/scenes/main.scene.json").c_str()));
    CHECK(std::string(actual::s_current_project_root) == project);
    CHECK(actual::canvas_root == project + "/content/source");
    REQUIRE(actual::follow_scene_project_root((b + "/scenes/main.scene.json").c_str()));
    CHECK(actual::s_current_project_root[0] == '\0');
    CHECK(actual::model_root == b);
    CHECK(actual::canvas_root == b);
    CHECK(actual::particle_root == b);
    CHECK(asset_browser_root == b);
    actual::consume_game_content_context();
    CHECK(actual::canvas_root == b);
}

TEST_CASE("Bundle without a manifest never retires its newly installed isolated mount")
{
    Fixture f;
    const auto author = f.loose("asset-folder-context"), package = f.loose("package");
    REQUIRE(actual::set_current_project_root(author.c_str()));
    REQUIRE_FALSE(actual::project);
    actual::active_fs = reinterpret_cast<JceFileSystem *>(uintptr_t{1});
    actual::active_policy = JCE_FS_ACTIVE_ISOLATED;
    actual::transitions = actual::publications = actual::bundle_retirements = 0;
    REQUIRE(actual::prepare_scene_project_switch((package + "/content.jbundle").c_str(), true));
    REQUIRE(actual::follow_scene_project_root((package + "/content.jbundle").c_str(), true));
    CHECK(actual::transitions == 0);
    CHECK(actual::publications == 0);
    CHECK(actual::bundle_retirements == 0);
    CHECK(actual::active_fs != nullptr);
    CHECK(actual::active_policy == JCE_FS_ACTIVE_ISOLATED);
    CHECK(std::string(actual::s_current_project_root) == author);
    actual::jce_editor_scene_render_refresh_content_context();
    CHECK(actual::canvas_root.empty());
    CHECK(actual::particle_root.empty());
}

TEST_CASE("actual Asset Browser entry leaves scene content ownership with its loader")
{
    Fixture f;
    const auto root = f.loose("asset-browser");
    const actual::FileEntry entry{false, root + "/scenes/main.scene.json"};
    actual::open_asset_entry(entry);
    CHECK(actual::scene_loads == 1);
    CHECK(actual::model_root == root);
    CHECK(actual::canvas_root == root);
    CHECK(actual::particle_root == root);
    const auto previous = actual::canvas_root;
    actual::scene_load_allowed = false;
    actual::open_asset_entry({false, root + "/missing/scenes/refused.scene.json"});
    CHECK(actual::scene_loads == 2);
    CHECK(actual::model_root == root);
    CHECK(actual::canvas_root == previous);
    CHECK(actual::particle_root == root);
    actual::asset_is_scene = false;
    actual::open_asset_entry({false, root + "/fonts/face.ttf"});
    CHECK(actual::file_opens == 1);
    actual::open_asset_entry({true, root});
    CHECK(actual::directory_opens == 1);
    CHECK(actual::scene_loads == 2);
}
