// Actual Scene wall-clock owner and four content call sites; CPU ports only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_state.h"
#include "scene/jce_editor_scene_render.h"
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/os/core/jce_timer.h>
#include <type_traits>
#include <vector>

static uint64_t now_ticks=140,frequency=1000;
static JcePlayState play_state=JCE_PLAY_STOPPED;
static float play_delta=0;
static unsigned play_reads;
struct Call { char sink; float dt; };
static std::vector<Call> calls;
alignas(void *) static unsigned char opaque;
template<class T> static T *token() { return reinterpret_cast<T *>(&opaque); }
struct {
    uint64_t anim_last_ticks=100;
    JceSceneRenderer *scene_renderer=token<JceSceneRenderer>();
    JceCamera *camera=token<JceCamera>();
    JceOffscreenTarget *bridge=token<JceOffscreenTarget>();
    JceUICanvas *ui_canvas=token<JceUICanvas>();
} s_sr;
static uint64_t JCE_CALL ticks_port() { return now_ticks; }
static uint64_t JCE_CALL frequency_port() { return frequency; }
static JcePlayState state_port() { return play_state; }
static float play_delta_port() { ++play_reads; return play_delta; }
static uint16_t scene_view_id() { return 3; }
static void navigation_port(float dt) { calls.push_back({'N',dt}); }
static void reflection_port(JceSceneRenderer *r,JceScene *s,const JceCamera *c,float dt)
{ CHECK(r==s_sr.scene_renderer);CHECK(s==token<JceScene>());CHECK(c==s_sr.camera);calls.push_back({'R',dt}); }
static uint16_t render_port(JceSceneRenderer *r,JceScene *s,const JceCamera *c,uint16_t view,float dt,const JceSceneRenderConfig *cfg)
{ CHECK(r==s_sr.scene_renderer);CHECK(s==token<JceScene>());CHECK(c==s_sr.camera);CHECK(view==3);CHECK(cfg!=nullptr);calls.push_back({'M',dt});return view; }
static bool fullscreen_port(JceSceneRenderer *r,JceScene *s,const JceCamera *c,JceOffscreenTarget *bridge,uint16_t view,int viewport,uint32_t width,uint32_t height,float dt)
{ CHECK(r==s_sr.scene_renderer);CHECK(s==token<JceScene>());CHECK(c==s_sr.camera);CHECK(bridge==s_sr.bridge);CHECK(view==3);CHECK(viewport==1);CHECK(width==1280);CHECK(height==720);calls.push_back({'F',dt});return true; }
static void canvas_port(JceUICanvas *canvas,JceScene *scene,uint16_t view,uint16_t frame,float width,float height,const JceUIPointer *pointer,float dt)
{ CHECK(canvas==s_sr.ui_canvas);CHECK(scene==token<JceScene>());CHECK(view==20);CHECK(frame==9);CHECK(width==1280);CHECK(height==720);CHECK(pointer==nullptr);calls.push_back({'C',dt}); }
static_assert(std::is_same_v<decltype(&ticks_port),decltype(&jce_time_perf_counter)>);
static_assert(std::is_same_v<decltype(&frequency_port),decltype(&jce_time_perf_freq)>);
static_assert(std::is_same_v<decltype(&reflection_port),decltype(&jce_scene_renderer_render_planar_reflection)>);
static_assert(std::is_same_v<decltype(&render_port),decltype(&jce_scene_renderer_render)>);
static_assert(std::is_same_v<decltype(&canvas_port),decltype(&jce_ui_canvas_render)>);
#define jce_time_perf_counter ticks_port
#define jce_time_perf_freq frequency_port
#define jce_state_get_play_state state_port
#define jce_editor_play_render_delta play_delta_port
#define jce_editor_scene_camera_update navigation_port
#define jce_scene_renderer_render_planar_reflection reflection_port
#define jce_scene_renderer_render render_port
#define jce_editor_viewport_apply_fullscreen_effects fullscreen_port
#define jce_ui_canvas_render canvas_port
#include JCE_EDITOR_SCENE_CONTENT_CLOCK_OWNER
#undef jce_time_perf_counter
#undef jce_time_perf_freq
#undef jce_state_get_play_state
#undef jce_editor_play_render_delta
#undef jce_editor_scene_camera_update
#undef jce_scene_renderer_render_planar_reflection
#undef jce_scene_renderer_render
#undef jce_editor_viewport_apply_fullscreen_effects
#undef jce_ui_canvas_render

struct Fixture {
    Fixture() { calls.clear();now_ticks=140;frequency=1000;s_sr.anim_last_ticks=100;play_state=JCE_PLAY_STOPPED;play_delta=0;play_reads=0; }
};
static void expect_scene(float wall,float content)
{
    REQUIRE(calls.size()==5);
    const char order[]={'N','R','M','F','C'};
    for(size_t i=0;i<5;++i) { CHECK(calls[i].sink==order[i]);CHECK(calls[i].dt==doctest::Approx(i?content:wall)); }
    CHECK(s_sr.anim_last_ticks==now_ticks);
}
TEST_CASE("STOPPED authoring content retains the original bounded wall preview")
{
    Fixture fixture;play_delta=.003f;
    test_actual_scene_content_frame(1280,720);expect_scene(.04f,.04f);CHECK(play_reads==0);
    calls.clear();now_ticks=740;test_actual_scene_content_frame(1280,720);expect_scene(.1f,.1f);
    calls.clear();s_sr.anim_last_ticks=0;now_ticks=1000;test_actual_scene_content_frame(1280,720);expect_scene(0,0);
}
TEST_CASE("PAUSED Scene content freezes while editor navigation keeps wall time")
{
    Fixture fixture;play_state=JCE_PLAY_PAUSED;play_delta=0;
    test_actual_scene_content_frame(1280,720);expect_scene(.04f,0);
    CHECK(test_actual_game_content_delta()==0);
    calls.clear();now_ticks=240;test_actual_scene_content_frame(1280,720);expect_scene(.1f,0);
}
TEST_CASE("PLAYING Scene consumes the same accepted scaled delta as Game")
{
    Fixture fixture;play_state=JCE_PLAY_PLAYING;play_delta=.00625f;
    const float game_first=test_actual_game_content_delta();
    test_actual_scene_content_frame(1280,720);expect_scene(.04f,game_first);
    CHECK(game_first==play_delta);CHECK(test_actual_game_content_delta()==game_first);
    calls.clear();now_ticks=180;play_delta=.0125f;
    test_actual_scene_content_frame(1280,720);expect_scene(.04f,test_actual_game_content_delta());
}
TEST_CASE("A first or newly revealed Scene view does not replace the Play delta")
{
    Fixture fixture;play_state=JCE_PLAY_PLAYING;play_delta=.016f;s_sr.anim_last_ticks=0;
    test_actual_scene_content_frame(1280,720);expect_scene(0,.016f);
    calls.clear();now_ticks=2140;test_actual_scene_content_frame(1280,720);expect_scene(.1f,.016f);
    calls.clear();play_delta=0;now_ticks=2180;test_actual_scene_content_frame(1280,720);expect_scene(.04f,0);
}
TEST_CASE("Pause and resume preserve every render sink and restore accepted time")
{
    Fixture fixture;play_state=JCE_PLAY_PLAYING;play_delta=.02f;
    test_actual_scene_content_frame(1280,720);expect_scene(.04f,.02f);
    calls.clear();now_ticks=180;play_state=JCE_PLAY_PAUSED;play_delta=0;
    test_actual_scene_content_frame(1280,720);expect_scene(.04f,0);
    calls.clear();now_ticks=220;play_state=JCE_PLAY_PLAYING;play_delta=.005f;
    test_actual_scene_content_frame(1280,720);expect_scene(.04f,.005f);
}
