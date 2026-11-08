#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_game_input_bridge.h"
#include <jce/os/platform/jce_input_record.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/middleware/scene/jce_scene.h>
#include <cmath>
#include <cstring>

namespace {
constexpr const char *record_path = "editor_pointer_replay.jirc";
constexpr const char *script_path = "editor_pointer_replay.lua";
struct Owners {
    JceInput *input = jce_input_create();
    JceScene *scene = nullptr;
    JceRuntime *runtime = nullptr;
    JceInputRecorder *reader = nullptr;
    ~Owners() {
        jce_input_record_close(reader);
        jce_runtime_destroy(runtime);
        jce_scene_destroy(scene);
        jce_input_destroy(input);
        jce_fs_host_remove_recursive(record_path);
        jce_fs_host_remove_recursive(script_path);
    }
};
JceEditorGameInputViewport viewport() {
    JceEditorGameInputViewport v{};
    v.x=100; v.y=50; v.width=640; v.height=360;
    v.render_width=1280; v.render_height=1080;
    v.visible=true; v.owner=JCE_EDITOR_GAME_INPUT_RUNTIME;
    return v; /* replay must not rewrite physical focus/hover */
}
void record_frames(JceInput *input) {
    auto *writer=jce_input_record_open(record_path); REQUIRE(writer);
    for (int i=0;i<3;++i) {
        JceInputFrame f{}; jce_input_capture(input,&f);
        f.mouse_x=420; f.mouse_y=230;
        f.mouse_dx=i==1?3.0f:0.0f; f.mouse_dy=i==1?-2.0f:0.0f;
        f.mouse_wheel=i==1?1.0f:0.0f;
        f.mouse_buttons=i<2?JCE_MOUSE_BUTTON_MASK(1):0;
        REQUIRE(jce_input_apply(input,&f));
        REQUIRE_FALSE(jce_input_snapshot_was_replayed(input));
        REQUIRE(jce_input_record_tick(writer,input));
    }
    jce_input_record_close(writer);
}
void pointer(void *ud,float dx,float dy,float wheel,uint32_t buttons) {
    jce_runtime_set_pointer_input(static_cast<Owners *>(ud)->runtime,dx,dy,wheel,buttons);
}
void position(void *ud,float x,float y,float w,float h) {
    jce_runtime_set_pointer_position(static_cast<Owners *>(ud)->runtime,x,y,w,h);
}
void touches(void *ud,const JceRuntimeTouch *t,int n) {
    jce_runtime_set_touch_input(static_cast<Owners *>(ud)->runtime,t,n);
}
void actions(void *,const JceInputActions *) {}
void step(void *ud,float dt) {jce_runtime_step(static_cast<Owners *>(ud)->runtime,dt);}
const JceEditorGameInputCommitOps ops={pointer,touches,actions,step,position};
JceEntity make_runtime(Owners &o) {
    const char script[]="local M={}\nfunction M:on_update(dt)\n"
        "local dx,dy=jce.get_pointer_delta()\n"
        "jce.set_position(self.entity,dx,dy,jce.is_pointer_down(1) and 1 or 0)\n"
        "end\nreturn M\n";
    REQUIRE(jce_fs_host_write_all(script_path,script,sizeof(script)-1));
    o.scene=jce_scene_create(); REQUIRE(o.scene);
    JceEntity e=jce_scene_create_entity(o.scene,"pointer probe");
    JceTransform t{}; t.position={-99,-99,-99}; t.rotation.w=1; t.scale={1,1,1};
    jce_scene_set_transform(o.scene,e,&t);
    JceScriptComponent sc{}; std::strncpy(sc.script_path,script_path,sizeof(sc.script_path)-1);
    jce_scene_set_script(o.scene,e,&sc);
    jce_scene_set_component_enabled(o.scene,e,JCE_COMP_FLAG_SCRIPT,true);
    JceRuntimeDesc desc{}; desc.scene=o.scene; desc.enable_physics=false;
    o.runtime=jce_runtime_create(&desc); REQUIRE(o.runtime); return e;
}
}

TEST_CASE("actual JIRC snapshot reaches the real Lua pointer host and release") {
    Owners o; REQUIRE(o.input); record_frames(o.input);
    JceEntity e=make_runtime(o); o.reader=jce_input_replay_open(record_path); REQUIRE(o.reader);
    JceEditorGameInputBridge bridge{}; auto v=viewport();
    jce_editor_game_input_bridge_publish(&bridge,&v);
    for (int frame=0;frame<3;++frame) {
        REQUIRE(jce_input_replay_tick(o.reader,o.input));
        CHECK(jce_input_mouse_button_pressed(o.input,1)==(frame==0));
        CHECK(jce_input_mouse_button_released(o.input,1)==(frame==2));
        jce_editor_game_input_bridge_sync_replay(&bridge,o.input);
        JceEditorGameInputSample sample{};
        REQUIRE(jce_editor_game_input_bridge_peek_virtual(&bridge,&v,&sample));
        CHECK(sample.replayed); CHECK(sample.synthetic);
        CHECK(sample.x==640); CHECK(sample.y==540);
        CHECK_FALSE(bridge.viewport.hovered); CHECK_FALSE(bridge.viewport.focused);
        REQUIRE(jce_editor_game_input_bridge_commit(&bridge,nullptr,0,nullptr,1.0f/60,&ops,&o));
        auto *t=jce_scene_get_transform(o.scene,e); REQUIRE(t);
        CHECK(t->position.x==doctest::Approx(frame==1?6:0));
        CHECK(t->position.y==doctest::Approx(frame==1?-6:0));
        CHECK(t->position.z==doctest::Approx(frame<2?1:0));
        /* A repeated commit must not reuse displacement, while held state stays. */
        REQUIRE(jce_editor_game_input_bridge_commit(&bridge,nullptr,0,nullptr,1.0f/60,&ops,&o));
        t=jce_scene_get_transform(o.scene,e); REQUIRE(t);
        CHECK(t->position.x==0); CHECK(t->position.y==0);
        CHECK(t->position.z==doctest::Approx(frame<2?1:0));
        jce_input_update(o.input);
    }
    CHECK_FALSE(jce_input_replay_tick(o.reader,o.input));
    jce_editor_game_input_bridge_sync_replay(&bridge,o.input);
    CHECK_FALSE(bridge.virtual_pointer.active);
    REQUIRE(jce_editor_game_input_bridge_commit(&bridge,nullptr,0,nullptr,1.0f/60,&ops,&o));
    auto *t=jce_scene_get_transform(o.scene,e); REQUIRE(t); CHECK(t->position.z==0);
    jce_runtime_step(o.runtime,1.0f/60);
    t=jce_scene_get_transform(o.scene,e); REQUIRE(t); CHECK(t->position.x==0); CHECK(t->position.z==0);
}

TEST_CASE("replay has current geometry and cannot mix live events or stale origin") {
    Owners o; REQUIRE(o.input); record_frames(o.input);
    o.reader=jce_input_replay_open(record_path); REQUIRE(o.reader);
    JceEditorGameInputBridge b{}; auto v=viewport(); v.focused=v.hovered=true;
    jce_editor_game_input_bridge_publish(&b,&v);
    JceEvent event{}; event.type=JCE_EVENT_MOUSE_MOTION;
    event.motion={430,240,90,80}; jce_editor_game_input_bridge_handle_event(&b,&event);
    REQUIRE(jce_input_replay_tick(o.reader,o.input));
    jce_editor_game_input_bridge_sync_replay(&b,o.input);
    auto s=jce_editor_game_input_bridge_consume(&b);
    CHECK(s.replayed); CHECK(s.dx==0); CHECK(s.dy==0); CHECK(s.buttons==1);
    v.render_width+=1; jce_editor_game_input_bridge_publish(&b,&v);
    CHECK_FALSE(b.virtual_pointer.active);
    v.visible=false; jce_editor_game_input_bridge_publish(&b,&v);
    jce_editor_game_input_bridge_sync_replay(&b,o.input);
    CHECK_FALSE(b.virtual_pointer.active);
    v=viewport(); jce_editor_game_input_bridge_publish(&b,&v);
    REQUIRE(jce_input_replay_tick(o.reader,o.input));
    jce_editor_game_input_bridge_sync_replay(&b,o.input);
    CHECK(b.virtual_pointer.replayed);
    JceInputFrame f{}; jce_input_capture(o.input,&f);
    REQUIRE(jce_input_apply(o.input,&f));
    jce_editor_game_input_bridge_sync_replay(&b,o.input);
    CHECK_FALSE(b.virtual_pointer.active);
    /* Explicit virtual fixtures remain independent when no replay is applied. */
    event.motion={420,230,1,2}; REQUIRE(jce_editor_game_input_bridge_handle_virtual_event(&b,&event));
    jce_editor_game_input_bridge_sync_replay(&b,o.input);
    s=jce_editor_game_input_bridge_consume(&b);
    CHECK(s.synthetic); CHECK_FALSE(s.replayed); CHECK(s.dx==2); CHECK(s.dy==6);
}
