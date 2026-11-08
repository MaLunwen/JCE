/* Exact production factory/activation bodies, counted failure ports only.
 * These do not claim an actual ECS allocation failure, OS UI or GPU result. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_state.h"
#include "core/jce_editor_project.h"
#include <jce/middleware/scene/jce_scene_camera.h>
#include <cstring>
#include <string>
#include <vector>

namespace {
int scene_a, scene_b;
struct SceneState { JceScene *scene = nullptr; bool scene_modified = false; } s;
struct Snapshot { std::string scene_json, scene_path; };
struct Transaction { bool active = false; char label[64]{}; Snapshot before; } s_transaction;
int s_history_edit_nesting = 0, s_history_manual_batch_depth = 0;
bool s_history_outer_edit_pushed_snapshot = false, s_suppress_add_component_log = false;
std::vector<unsigned> g_entity_order;
std::vector<std::string> calls;
std::string current_path, scene_dir;
int history_entries, destroyed, created;
bool create_ok, builder_ok, primary_ok, selected_2d, root_ok;
JcePlayState play;
JceProject cached;

void detached(void)
{
    CHECK(current_path.empty());
    CHECK(scene_dir.empty());
    CHECK(history_entries == 0);
    CHECK_FALSE(s_transaction.active);
    CHECK(s_transaction.label[0] == '\0');
    CHECK(s_transaction.before.scene_json.empty());
    CHECK(s_transaction.before.scene_path.empty());
    CHECK(s_history_edit_nesting == 0);
    CHECK_FALSE(s_history_outer_edit_pushed_snapshot);
    CHECK(s_history_manual_batch_depth == 0);
    CHECK(s.scene_modified); /* Before create/destroy, and on every failed path. */
}
void reset(bool existing = true)
{
    s.scene = existing ? reinterpret_cast<JceScene *>(&scene_a) : nullptr;
    s.scene_modified = false;
    current_path = "project-A/assets/Old.scene"; scene_dir = "project-A/assets";
    history_entries = 3; s_history_edit_nesting = 1;
    s_history_outer_edit_pushed_snapshot = true; s_history_manual_batch_depth = 1;
    s_transaction.active = true; std::strcpy(s_transaction.label, "A edit");
    s_transaction.before.scene_json = "A authored snapshot";
    s_transaction.before.scene_path = current_path;
    destroyed = created = 0; create_ok = builder_ok = primary_ok = root_ok = true;
    selected_2d = false; play = JCE_PLAY_PLAYING; calls.clear();
    g_entity_order.clear(); s_suppress_add_component_log = false;
}
struct HistorySuspendScope { HistorySuspendScope() {} ~HistorySuspendScope() {} };
bool stop_play_before_scene_swap(void) { calls.emplace_back("stop"); play=JCE_PLAY_STOPPED; return true; }
void jce_editor_scene_render_streaming_teardown(void) { calls.emplace_back("stream"); }
void jce_editor_scene_render_streaming_rebuild(void) { calls.emplace_back("rebuild"); }
void set_current_scene_path_internal(const char *path) { calls.emplace_back("path"); current_path=path?path:""; }
void jce_editor_scene_set_scene_dir(const char *path) { calls.emplace_back("directory"); scene_dir=path?path:""; }
void clear_scene_entities(void) { detached(); ++destroyed; calls.emplace_back("destroy"); s.scene=jce_scene_create(); }
bool build_default_2d_scene(void) { calls.emplace_back("2d"); return builder_ok; }
void build_demo_scene(void) { calls.emplace_back("3d"); }
void jce_editor_scene_ensure_rendering_settings(JceScene *) { calls.emplace_back("settings"); }
void jce_editor_assets_set_project(const char *) { CHECK(play==JCE_PLAY_STOPPED); calls.emplace_back("assets"); }
bool set_current_project_root(const char *) { CHECK(play==JCE_PLAY_STOPPED); calls.emplace_back("root-and-modules"); return root_ok; }
}
bool jce_state_close_bundle_preview(void) { calls.emplace_back("bundle"); return true; }
void jce_state_history_clear(void) { calls.emplace_back("history"); history_entries=0; }
void jce_state_set_2d_mode(bool is_2d) { selected_2d=is_2d; }
JcePlayState jce_state_get_play_state(void) { return play; }
void jce_state_stop(void) { calls.emplace_back("actual-stop"); play=JCE_PLAY_STOPPED; }
const JceProject *jce_editor_project_get(void) { return &cached; }
extern "C" JceScene *jce_scene_create(void)
{
    detached(); ++created; calls.emplace_back("create");
    return create_ok ? reinterpret_cast<JceScene *>(&scene_b) : nullptr;
}
extern "C" JceSceneCameraResolveResult jce_scene_camera_resolve_primary(JceScene *,JceSceneCameraPose *)
{
    calls.emplace_back("primary");
    return primary_ok ? JCE_SCENE_CAMERA_RESOLVE_OK : JCE_SCENE_CAMERA_RESOLVE_NOT_FOUND;
}
#ifdef LOG_ERROR
#undef LOG_ERROR
#endif
#ifdef LOG_INFO
#undef LOG_INFO
#endif
#define LOG_ERROR(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#include JCE_TEST_EDITOR_NEW_SCENE_BODY
#include JCE_TEST_EDITOR_NEW_PROJECT_ACTIVATION_BODY

TEST_CASE("create failure after teardown cannot retain A's save or undo identity")
{
    for(bool existing:{false,true}) {
        reset(existing); create_ok=false;
        CHECK_FALSE(jce_state_new_default_scene_for_project(false));
        detached(); CHECK(created==1); CHECK(destroyed==(existing?1:0));
        REQUIRE(calls.size()>=3);
        CHECK(calls[0]=="stop"); CHECK(calls[1]=="stream"); CHECK(calls[2]=="bundle");
        CHECK_FALSE(s_suppress_add_component_log);
    }
}
TEST_CASE("3D primary failure leaves B dirty without A's save or transaction")
{
    reset(); primary_ok=false;
    CHECK_FALSE(jce_state_new_default_scene_for_project(false));
    detached(); CHECK(destroyed==1); CHECK(created==1);
    CHECK_FALSE(s_suppress_add_component_log);
}
TEST_CASE("2D builder failure has the same detached partial-state boundary")
{
    reset(); builder_ok=false;
    CHECK_FALSE(jce_state_new_default_scene_for_project(true));
    detached(); CHECK(destroyed==1); CHECK(created==1);
    CHECK_FALSE(s_suppress_add_component_log);
}
TEST_CASE("successful fresh scene keeps the existing completion and selected mode")
{
    for(bool is_2d:{false,true}) {
        reset(); REQUIRE(jce_state_new_default_scene_for_project(is_2d));
        CHECK(current_path.empty()); CHECK(scene_dir.empty()); CHECK(history_entries==0);
        CHECK_FALSE(s_transaction.active); CHECK_FALSE(s.scene_modified);
        CHECK(selected_2d==is_2d); CHECK(destroyed==1); CHECK(created==1);
        CHECK(calls.back()=="rebuild");
    }
}
TEST_CASE("actual project activation stops live Play before repoint and module reload")
{
    reset(); REQUIRE(activate_new_project("project-B"));
    const std::vector<std::string> running={"actual-stop","root-and-modules","assets"};
    CHECK(calls==running);
    calls.clear(); REQUIRE(activate_new_project("project-B"));
    const std::vector<std::string> stopped={"root-and-modules","assets"};
    CHECK(calls==stopped);
}

TEST_CASE("actual project activation refuses root failure before asset repoint")
{
    reset(); root_ok=false;
    CHECK_FALSE(activate_new_project("project-B"));
    const std::vector<std::string> refused={"actual-stop","root-and-modules"};
    CHECK(calls==refused);
    CHECK(current_path=="project-A/assets/Old.scene");
    CHECK(destroyed==0); CHECK(created==0);
}
