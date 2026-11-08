#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_play_viewport.h"
#include "core/jce_editor_cpu_scope.h"
#include <cstdint>
#include <cstring>
struct Runtime {float width=0,height=0;} runtime;
struct {int play_state=0;} s;
struct {JceEditorPlayViewport viewport;struct {Runtime *runtime=nullptr;} runtime_stats;} s_play_host;
enum {JCE_PLAY_STOPPED=0,JCE_PLAY_PLAYING=1};
static unsigned begins,focus_requests,logs,resolves,sets;
static float scale=1;
#define LOG_TAG "test"
#define LOG_INFO(...) (++logs)
static void jce_editor_layout_request_focus_game_view(){++focus_requests;}
static void jce_render_pipeline_scene_extent(uint32_t w,uint32_t h,uint32_t *ow,uint32_t *oh)
{++resolves;*ow=(uint32_t)(w*scale);*oh=(uint32_t)(h*scale);}
static bool jce_runtime_set_viewport_size(Runtime *rt,float w,float h)
{++sets;rt->width=w;rt->height=h;return true;}
static void play_begin_ready()
{++begins;s.play_state=JCE_PLAY_PLAYING;s_play_host.runtime_stats.runtime=&runtime;}
#ifndef JCE_EDITOR_PLAY_VIEWPORT_WIRE_HEADER
#define JCE_EDITOR_PLAY_VIEWPORT_WIRE_HEADER "jce_editor_play_viewport_wire.inc"
#endif
#include JCE_EDITOR_PLAY_VIEWPORT_WIRE_HEADER
static void reset()
{s.play_state=JCE_PLAY_STOPPED;s_play_host={};runtime={};begins=focus_requests=logs=resolves=sets=0;scale=1;}
TEST_CASE("Play waits for actual layout and repeated requests coalesce")
{
    reset();jce_state_play();jce_state_play();
    CHECK(s_play_host.viewport.pending);CHECK(begins==0);CHECK(focus_requests==1);CHECK(logs==1);
    jce_editor_play_publish_viewport(0,650);CHECK(begins==0);CHECK(sets==0);
    jce_editor_play_publish_viewport(909,650);
    CHECK(begins==1);CHECK_FALSE(s_play_host.viewport.pending);
    CHECK(runtime.width==909);CHECK(runtime.height==650);
    jce_editor_play_publish_viewport(909,650);CHECK(begins==1);
}
TEST_CASE("Published extent uses the actual resolver regardless of pointer or focus")
{
    reset();scale=.5f;jce_state_play();jce_editor_play_publish_viewport(1280,720);
    CHECK(begins==1);CHECK(runtime.width==640);CHECK(runtime.height==360);
    jce_editor_play_publish_viewport(900,600);
    CHECK(runtime.width==450);CHECK(runtime.height==300);CHECK(begins==1);
    CHECK(resolves==2);CHECK(sets==2);
}
TEST_CASE("Stopped preview publication cannot create a runtime")
{
    reset();jce_editor_play_publish_viewport(800,600);
    CHECK(begins==0);CHECK(sets==0);CHECK_FALSE(s_play_host.viewport.pending);
    CHECK(s_play_host.viewport.width==800);
}
TEST_CASE("Invalid layout and failed begin never invent dimensions or retry a creation")
{
    reset();jce_state_play();jce_editor_play_publish_viewport(UINT32_MAX,600);
    CHECK(begins==0);CHECK(sets==0);CHECK(s_play_host.viewport.width==0);
    JceEditorPlayViewport state{};jce_editor_play_viewport_request(state);
    CHECK(jce_editor_play_viewport_publish(state,800,600));
    unsigned failures=0;
    CHECK(jce_editor_play_viewport_begin_pending(state,[&]{++failures;}));
    CHECK_FALSE(jce_editor_play_viewport_begin_pending(state,[&]{++failures;}));
    CHECK(failures==1);
}
