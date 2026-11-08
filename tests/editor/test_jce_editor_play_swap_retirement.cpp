/* Actual Stop, capture and full-scene history restore bodies. Real ECS/codec,
 * real identity budget hook and real isolated FS; UI/render borrowers are ports.
 * This does not initialize a renderer, runtime VM or desktop. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_state_internal.h"
#include "core/jce_editor_history_identity.h"
#include "core/jce_editor_play_transaction.h"
#include "core/jce_editor_play_presentation.h"
#include "core/jce_editor_play_module.h"
#include "core/jce_editor_operation_metrics.h"
#include "core/jce_editor_project_context.h"
#include <jce/application/jce_runtime_host_policy.inc.h>
#include <jce/application/jce_runtime_stats_host.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_lighting_system.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_hash.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_set>

EditorInternalState s{};
std::vector<uint32_t> g_entity_order;
uint64_t g_entity_order_gen;
std::unordered_map<uint32_t, EditorEntitySidecar> g_entity_sidecar;
int s_history_suspend_depth;
static JceEditorHistoryIdentity s_history_identity;
static EditorHistorySnapshot s_play_snapshot;
static bool s_play_snapshot_valid;
static struct {
    JceRuntimeStatsHost runtime_stats{};
    JceEditorPlayPresentationState presentation{};
    JceEditorPlayModule module{};
    JceRuntimeHostPolicy policy{};
    struct { bool pending; } viewport;
    float render_dt;
} s_play_host;
static JceAudio *s_play_audio;
static bool s_play_resync_clock;
static int s_active_contacts;
static std::string root_name, budget;
static JceFileSystem *bundle_fs;
static unsigned mount_closes, stream_teardowns, root_commits, stop_drains;
static std::unordered_map<uint32_t, JceEntity> handles;

static const char *history_env(const char *key)
{
    if (!std::strcmp(key,"JCE_EDITOR_OPERATION_METRICS")) return "1";
    if (!std::strcmp(key,"JCE_EDITOR_HISTORY_IDENTITY_BUDGET_BYTES")) return budget.c_str();
    return nullptr;
}
JceEntity jce_state_to_ecs_entity(uint32_t id)
{ auto it=handles.find(id); return it==handles.end()?0:it->second; }
void rebuild_entity_order_from_ecs(void)
{
    handles.clear(); g_entity_order.clear();
    jce_scene_each_entity(s.scene, [](JceScene *, JceEntity e, void *) {
        handles[(uint32_t)e]=e; g_entity_order.push_back((uint32_t)e);
    },nullptr);
}
void jce_roots_invalidate(void) {}
void jce_state_clear_selection(void) { s.selected_count=0; s.focused=0; }
void jce_state_select_entity(uint32_t id, bool) { s.selected[s.selected_count++]=id; }
bool jce_state_is_selected(uint32_t id)
{ return std::find(s.selected,s.selected+s.selected_count,id)!=s.selected+s.selected_count; }
void jce_state_set_focused(uint32_t id) { s.focused=id; }
uint32_t jce_state_get_focused(void) { return s.focused; }
uint32_t jce_state_entity_parent(uint32_t) { return 0; }
const char *jce_state_entity_name(uint32_t id) { return jce_scene_entity_name(s.scene,jce_state_to_ecs_entity(id)); }
int jce_state_get_entity_count(void) { return (int)g_entity_order.size(); }
void set_current_scene_path_internal(const char *path)
{ std::snprintf(s.current_scene_path,sizeof s.current_scene_path,"%s",path?path:""); }
void jce_editor_scene_render_cancel_probe_capture(void) {}
void jce_editor_scene_render_reset_occlusion(void) {}
void jce_editor_game_render_reset_occlusion(void) {}
void jce_editor_inspector_request_sync(void) {}
static bool history_restore_entity_snapshot(const EditorHistorySnapshot &, const char *)
{ FAIL("Play must use the actual full-scene restore"); return false; }
#define getenv history_env
#include JCE_TEST_PLAY_SWAP_HISTORY
#undef getenv

JceSceneRenderer *jce_editor_get_scene_renderer(void) { return nullptr; }
void jce_editor_game_render_end_play_camera(void) {}
void jce_editor_game_render_set_mouse_capture(bool) {}
void jce_editor_game_render_set_play_runtime(JceRuntime *) {}
void jce_editor_scene_reset_anim_timer(void) {}
void jce_editor_scene_render_streaming_rebuild(void) {}
static void play_streaming_end(void) { ++stop_drains; }
#include JCE_TEST_PLAY_SWAP_STOP

void jce_editor_scene_render_streaming_teardown(void) { ++stream_teardowns; }
static void close_active_bundle_mount(void)
{
    ++mount_closes;
    if (bundle_fs) { jce_fs_set_active_policy(nullptr,JCE_FS_ACTIVE_OVERLAY); jce_fs_destroy(bundle_fs); bundle_fs=nullptr; }
}
#include JCE_TEST_PLAY_SWAP_CLOSE

/* Actual project transition prefix through committed root publication. Engine
 * scene recovery is real above; project persistence/subsystems are typed ports. */
const char *jce_editor_current_project_root(void) { return root_name.c_str(); }
bool jce_editor_prepare_project_switch(const char *) { return true; }
void jce_editor_console_log_level(int, const char *, ...) {}
static constexpr int JCE_CONSOLE_ERROR=2;
bool jce_editor_current_project_root_set(const char *root)
{ ++root_commits; root_name=root?root:""; return true; }
void jce_project_settings_set_root(const char *) {}
void jce_assetdb_set_root(const char *) {}
void jce_editor_project_set_root(const char *) {}
void jce_editor_script_modules_reload(const char *) {}
void jce_editor_gl10n_unload(void) {}
void jce_pak_key_install_process(const char *) {}
#include JCE_TEST_PLAY_SWAP_PROJECT

static unsigned scene_commits;
void jce_panel_sequencer_preview_flush(void) {}
static bool bundle_path_from_sidecar(const char *, char *, size_t) { return false; }
bool parse_scene_contract_version(const JceJson *, int *, int *) { return true; }
#include JCE_TEST_PLAY_SWAP_ENTRIES

static std::string scene_bytes()
{
    size_t n=0;char *p=jce_scene_serial_save(s.scene,&n);REQUIRE(p!=nullptr);
    std::string result(p,n);jce_json_free_string(p);return result;
}
struct Fixture {
    Fixture()
    {
        jce_trace_set_enabled(false);jce_trace_reset();
        s={};s_play_host={};s_play_snapshot={};s_play_snapshot_valid=false;
        s_history_identity=JceEditorHistoryIdentity();s_play_audio=nullptr;
        s_play_resync_clock=false;s_active_contacts=0;s_history_suspend_depth=0;
        mount_closes=stream_teardowns=root_commits=stop_drains=scene_commits=0;
        root_name="A";budget="67108864";
        s.scene=jce_scene_create();REQUIRE(s.scene!=nullptr);
        bundle_fs=jce_fs_create();REQUIRE(bundle_fs!=nullptr);
        jce_fs_set_active_policy(bundle_fs,JCE_FS_ACTIVE_ISOLATED);
        auto e=jce_scene_create_entity(s.scene,"A-authored");REQUIRE(e!=0);
        rebuild_entity_order_from_ecs();REQUIRE(s_history_identity.token(e)!=0);
        set_current_scene_path_internal("A/authored.jcescene");
        REQUIRE(history_capture_snapshot(&s_play_snapshot));s_play_snapshot_valid=true;
        jce_scene_set_entity_name(s.scene,e,"A-simulated");
    }
    ~Fixture()
    {
        jce_fs_set_active_policy(nullptr,JCE_FS_ACTIVE_OVERLAY);
        if(bundle_fs)jce_fs_destroy(bundle_fs);bundle_fs=nullptr;
        jce_scene_destroy(s.scene);s.scene=nullptr;
        jce_trace_thread_unregister();jce_trace_set_enabled(false);jce_trace_reset();
    }
    void refuse() { budget="64"; }
    void allow() { budget="67108864"; }
    void preserved(const std::string &before,JceFileSystem *fs)
    {
        CHECK(root_name=="A");CHECK(root_commits==0);CHECK(mount_closes==0);
        CHECK(stream_teardowns==0);CHECK(bundle_fs==fs);CHECK(s_play_snapshot_valid);
        CHECK(scene_bytes()==before);CHECK(std::string(s.current_scene_path)=="A/authored.jcescene");
        CHECK(s.play_state==JCE_PLAY_STOPPED);CHECK_FALSE(s_play_host.viewport.pending);
    }
};
TEST_CASE("stopped retained restore blocks root and mount then retries real scene recovery")
{
    Fixture f;s.play_state=JCE_PLAY_STOPPED;f.refuse();
    const auto before=scene_bytes();auto *fs=bundle_fs;
    CHECK_FALSE(set_current_project_root("B"));f.preserved(before,fs);
    CHECK_FALSE(jce_state_close_bundle_preview());f.preserved(before,fs);
    f.allow();REQUIRE(set_current_project_root("B"));
    CHECK(root_name=="B");CHECK(root_commits==1);CHECK(mount_closes==1);
    CHECK_FALSE(s_play_snapshot_valid);CHECK(bundle_fs==nullptr);
    CHECK(scene_bytes().find("A-authored")!=std::string::npos);
    CHECK(scene_bytes().find("A-simulated")==std::string::npos);
    const auto recovered=scene_bytes();jce_state_stop();CHECK(scene_bytes()==recovered);
}
TEST_CASE("playing first failed restore drains runtime borrowers but retains switch ownership")
{
    Fixture f;s.play_state=JCE_PLAY_PLAYING;f.refuse();
    const auto before=scene_bytes();auto *fs=bundle_fs;
    CHECK_FALSE(set_current_project_root("B"));f.preserved(before,fs);
    CHECK(stop_drains==1);f.allow();REQUIRE(set_current_project_root("B"));
    CHECK(stop_drains==1);CHECK_FALSE(s_play_snapshot_valid);
    CHECK(scene_bytes().find("A-authored")!=std::string::npos);
}
TEST_CASE("pending request with no restore point retires without scene replacement")
{
    Fixture f;s_play_snapshot={};s_play_snapshot_valid=false;
    s.play_state=JCE_PLAY_STOPPED;s_play_host.viewport.pending=true;
    const auto before=scene_bytes();REQUIRE(set_current_project_root("B"));
    CHECK_FALSE(s_play_host.viewport.pending);CHECK(stop_drains==0);
    CHECK(scene_bytes()==before);CHECK(root_commits==1);CHECK(mount_closes==1);
}
TEST_CASE("terminal shutdown explicitly discards failed recovery only at final state destruction")
{
    Fixture f;s.play_state=JCE_PLAY_STOPPED;f.refuse();
    const auto before=scene_bytes();CHECK_FALSE(stop_play_before_scene_swap());
    CHECK(s_play_snapshot_valid);CHECK(mount_closes==0);
    jce_trace_set_enabled(true);jce_trace_thread_register("play-final-retirement");
    jce_editor_play_finish_shutdown();CHECK_FALSE(s_play_snapshot_valid);
    JceTraceCursor cursor;jce_trace_cursor_init(&cursor);JceTraceEvent events[128];
    const auto count=jce_trace_read(&cursor,events,128);unsigned final_ends=0;
    for(uint32_t i=0;i<count;++i)
        if(events[i].type==JCE_TRACE_EVENT_TASK_END &&
           !std::strcmp(events[i].name,"editor.play.finish_shutdown")) {
            ++final_ends;CHECK(events[i].value_u32[0]==JCE_TRACE_TASK_FAILED);
        }
    CHECK(final_ends==1);CHECK(cursor.lost_events==0);
    CHECK(s_play_snapshot.scene_json.empty());CHECK(scene_bytes()==before);
    CHECK(root_name=="A");CHECK(root_commits==0);
    REQUIRE(jce_state_close_bundle_preview());CHECK(mount_closes==1);
}

TEST_CASE("all destructive scene entries preserve retained recovery on refusal")
{
    using Entry=bool(*)();
    const Entry entries[]={
        +[]{return jce_state_load_scene_file("B/scene.jcescene");},
        +[]{return jce_state_load_scene_file_async("B/scene.jcescene");},
        +[]{return apply_scene_bytes("B", "{}",2);},
        +[]{return jce_state_load_scene_from_jbundle("B.jbundle");},
        +[]{return jce_state_load_scene_from_catalog("B/catalog.json",nullptr);},
        +[]{auto *root=jce_json_object();bool ok=load_scene_from_parsed_root(root,"B",nullptr);jce_json_free(root);return ok;},
        +[]{return jce_state_new_default_scene_for_project(false);}
    };
    for(auto enter:entries){
        Fixture f;s.play_state=JCE_PLAY_STOPPED;f.refuse();
        const auto before=scene_bytes();auto *fs=bundle_fs;
        CHECK_FALSE(enter());f.preserved(before,fs);CHECK(scene_commits==0);
        f.allow();REQUIRE(enter());CHECK_FALSE(s_play_snapshot_valid);CHECK(scene_commits==1);
        CHECK(scene_bytes().find("A-authored")!=std::string::npos);
    }
}

TEST_CASE("terminal finish stops only outstanding Play ownership")
{
    Fixture f;
    unsigned expected_stops=0;
    bool stop_before_terminal=false, expect_restore=false;
    SUBCASE("never started and no restore point") {
        s_play_snapshot={};s_play_snapshot_valid=false;s.play_state=JCE_PLAY_STOPPED;
    }
    SUBCASE("already retired by the actual Stop") {
        s.play_state=JCE_PLAY_PLAYING;stop_before_terminal=true;
        expected_stops=1;expect_restore=true;
    }
    SUBCASE("pending request without runtime or restore point") {
        s_play_snapshot={};s_play_snapshot_valid=false;s.play_state=JCE_PLAY_STOPPED;
        s_play_host.viewport.pending=true;expected_stops=1;
    }
    SUBCASE("active Play at final shutdown") {
        s.play_state=JCE_PLAY_PLAYING;expected_stops=1;expect_restore=true;
    }
    const auto before=scene_bytes();
    jce_trace_set_enabled(true);jce_trace_thread_register("play-terminal-noop");
    if(stop_before_terminal)jce_state_stop();
    jce_editor_play_finish_shutdown();
    const auto after_first=scene_bytes();
    jce_editor_play_finish_shutdown();
    CHECK(scene_bytes()==after_first);
    CHECK(s.play_state==JCE_PLAY_STOPPED);CHECK_FALSE(s_play_host.viewport.pending);
    CHECK_FALSE(s_play_snapshot_valid);CHECK(s_play_snapshot.scene_json.empty());
    CHECK(root_name=="A");CHECK(root_commits==0);CHECK(mount_closes==0);
    CHECK(stop_drains==(expect_restore?1u:0u));
    if(expect_restore) {
        CHECK(after_first.find("A-authored")!=std::string::npos);
        CHECK(after_first.find("A-simulated")==std::string::npos);
    } else CHECK(after_first==before);
    JceTraceCursor cursor;jce_trace_cursor_init(&cursor);JceTraceEvent events[512];
    const auto count=jce_trace_read(&cursor,events,512);
    unsigned stop_begins=0,stop_ends=0,finish_begins=0,finish_ends=0;
    for(uint32_t i=0;i<count;++i) {
        const auto &event=events[i];
        if(!std::strcmp(event.name,"editor.play.stop")) {
            if(event.type==JCE_TRACE_EVENT_TASK_BEGIN)++stop_begins;
            if(event.type==JCE_TRACE_EVENT_TASK_END) {
                ++stop_ends;CHECK(event.value_u32[0]==JCE_TRACE_TASK_SUCCEEDED);
            }
        } else if(!std::strcmp(event.name,"editor.play.finish_shutdown")) {
            if(event.type==JCE_TRACE_EVENT_TASK_BEGIN)++finish_begins;
            if(event.type==JCE_TRACE_EVENT_TASK_END) {
                ++finish_ends;CHECK(event.value_u32[0]==JCE_TRACE_TASK_SUCCEEDED);
            }
        }
    }
    CHECK(count<512);CHECK(cursor.lost_events==0);
    CHECK(stop_begins==expected_stops);CHECK(stop_ends==expected_stops);
    CHECK(finish_begins==2);CHECK(finish_ends==2);
}
