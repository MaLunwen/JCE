// Complete production DrawLog init/update blocks; typed renderer ports, real file IO.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor.h"
#include "scene/jce_editor_scene_render.h"
#include "jce_test_owned_directory.h"
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_str.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#define LOG_TAG "test.draw_log"

struct {
    char frame_kpi_path[1024]{};
    uint32_t frame_kpi_index=0,frame_kpi_limit=0;
    bool frame_kpi_shot_done=false;
} s_editor;
static bool stats_available=true,renderer_available=true;
static uint32_t stats_draw=23;
static unsigned stats_calls,rq_calls,cull_calls,scene_shots,backbuffer_shots;
alignas(void *) static unsigned char renderer_token;
static JceSceneRenderer *renderer_port()
{ return renderer_available ? reinterpret_cast<JceSceneRenderer *>(&renderer_token) : nullptr; }
static bool stats_port(JceGpuStats *out)
{
    REQUIRE(out!=nullptr);++stats_calls;*out={};out->valid=stats_available;
    out->num_draw=stats_draw;return stats_available;
}
static void rq_port(const JceSceneRenderer *sr,JceSceneRqStats *out)
{
    REQUIRE(sr==renderer_port());REQUIRE(out!=nullptr);++rq_calls;*out={};
    out->commands_in=11;out->submits_out=7;out->enabled=true;
}
static void cull_port(const JceSceneRenderer *sr,JceSceneCullStats *out)
{
    REQUIRE(sr==renderer_port());REQUIRE(out!=nullptr);++cull_calls;*out={};
    out->total=31;out->visible=13;out->disabled_skipped=2;
}
static bool scene_shot_port(const char *path)
{ CHECK(path!=nullptr);++scene_shots;return false; }
static bool backbuffer_shot_port(const char *path,bool interactive,unsigned source)
{ CHECK(path!=nullptr);CHECK_FALSE(interactive);CHECK(source==1);++backbuffer_shots;return true; }
static_assert(std::is_same_v<decltype(&stats_port),decltype(&jce_renderer_get_gpu_stats)>);
static_assert(std::is_same_v<decltype(&rq_port),decltype(&jce_scene_renderer_get_rq_stats)>);
static_assert(std::is_same_v<decltype(&cull_port),decltype(&jce_scene_renderer_get_cull_stats)>);
static_assert(std::is_same_v<decltype(&renderer_port),decltype(&jce_editor_get_scene_renderer)>);
static_assert(std::is_same_v<decltype(&scene_shot_port),decltype(&jce_editor_scene_render_screenshot)>);
static_assert(std::is_same_v<decltype(&backbuffer_shot_port),decltype(&jce_editor_request_screenshot)>);
#define jce_renderer_get_gpu_stats stats_port
#define jce_editor_get_scene_renderer renderer_port
#define jce_scene_renderer_get_rq_stats rq_port
#define jce_scene_renderer_get_cull_stats cull_port
#define jce_editor_scene_render_screenshot scene_shot_port
#define jce_editor_request_screenshot backbuffer_shot_port
#include JCE_EDITOR_DRAW_LOG_OWNER
#undef jce_renderer_get_gpu_stats
#undef jce_editor_get_scene_renderer
#undef jce_scene_renderer_get_rq_stats
#undef jce_scene_renderer_get_cull_stats
#undef jce_editor_scene_render_screenshot
#undef jce_editor_request_screenshot

static const std::string expected_header=
    "frame_index,frame_ms,num_draw,rq_commands,rq_submits,rq_enabled,"
    "cull_total,cull_visible,cull_disabled,gpu_stats_available\n";
static std::string read_file(const std::string &path)
{
    std::FILE *file=std::fopen(path.c_str(),"rb");REQUIRE(file!=nullptr);
    std::string result;char bytes[256];size_t count;
    while((count=std::fread(bytes,1,sizeof(bytes),file))!=0)result.append(bytes,count);
    CHECK(std::ferror(file)==0);CHECK(std::fclose(file)==0);
    std::printf("[DrawLog real FILE readback]\n%s[End DrawLog readback]\n",result.c_str());
    return result;
}
struct Fixture {
    JceTestOwnedDirectory directory{"_ut_editor_draw_log"};
    std::string path=directory.path("draw.csv");
    JceTestEnvironment draw{"JCE_KPI_DRAW_LOG",nullptr};
    JceTestEnvironment count{"JCE_KPI_DRAW_COUNT",nullptr};
    JceTestEnvironment shot{"JCE_KPI_SHOT",nullptr};
    Fixture()
    {
        s_editor={};stats_available=true;renderer_available=true;stats_draw=23;
        stats_calls=rq_calls=cull_calls=scene_shots=backbuffer_shots=0;
    }
    void start() { draw.set(path.c_str());test_actual_draw_log_init(); }
};
TEST_CASE("Absent or empty DrawLog leaves the real file and renderer untouched")
{
    Fixture f;
    for(const char *value:{static_cast<const char *>(nullptr),""}) {
        f.draw.set(value);test_actual_draw_log_init();test_actual_draw_log_update(.0125f);
        CHECK(s_editor.frame_kpi_path[0]=='\0');CHECK(s_editor.frame_kpi_index==0);
        CHECK_FALSE(jce_fs_host_exists_file(f.path.c_str()));
        CHECK(stats_calls==0);CHECK(rq_calls==0);CHECK(cull_calls==0);
    }
}
TEST_CASE("DrawLog appends availability after legacy columns and distinguishes real zero")
{
    Fixture f;f.start();CHECK(read_file(f.path)==expected_header);
    stats_available=false;stats_draw=99;test_actual_draw_log_update(.0125f);
    CHECK(stats_calls==1);CHECK(rq_calls==1);CHECK(cull_calls==1);
    const std::string unavailable="0,12.500,0,11,7,1,31,13,2,0\n";
    CHECK(read_file(f.path)==expected_header+unavailable);
    stats_available=true;stats_draw=0;test_actual_draw_log_update(.0125f);
    CHECK(stats_calls==2);CHECK(rq_calls==2);CHECK(cull_calls==2);
    const std::string zero="1,12.500,0,11,7,1,31,13,2,1\n";
    CHECK(read_file(f.path)==expected_header+unavailable+zero);
    stats_draw=23;test_actual_draw_log_update(.0125f);
    CHECK(stats_calls==3);CHECK(rq_calls==3);CHECK(cull_calls==3);
    CHECK(read_file(f.path)==expected_header+unavailable+zero+"2,12.500,23,11,7,1,31,13,2,1\n");
    CHECK(s_editor.frame_kpi_index==3);
}
TEST_CASE("Missing shared scene renderer does not erase available whole-frame draws")
{
    Fixture f;renderer_available=false;f.start();test_actual_draw_log_update(.0125f);
    CHECK(stats_calls==1);CHECK(rq_calls==0);CHECK(cull_calls==0);
    CHECK(read_file(f.path)==expected_header+"0,12.500,23,0,0,0,0,0,0,1\n");
}
TEST_CASE("Draw count limit does not add a stats query or row after completion")
{
    Fixture f;f.count.set("2");f.start();
    test_actual_draw_log_update(.0125f);test_actual_draw_log_update(.0125f);
    const std::string before=read_file(f.path);
    test_actual_draw_log_update(.0125f);test_actual_draw_log_update(.0125f);
    CHECK(stats_calls==2);CHECK(rq_calls==2);CHECK(cull_calls==2);
    CHECK(s_editor.frame_kpi_index==2);CHECK(s_editor.frame_kpi_shot_done);
    CHECK(scene_shots==0);CHECK(backbuffer_shots==0);CHECK(read_file(f.path)==before);
}
TEST_CASE("Explicit optional screenshot retains its existing one-shot admission")
{
    Fixture f;f.count.set("1");f.shot.set("explicit-only.png");f.start();
    test_actual_draw_log_update(.0125f);test_actual_draw_log_update(.0125f);
    test_actual_draw_log_update(.0125f);
    CHECK(stats_calls==1);CHECK(scene_shots==1);CHECK(backbuffer_shots==1);
}
TEST_CASE("Failed real header write cannot activate DrawLog or query stats")
{
    Fixture f;f.draw.set(f.directory.root.c_str());
    test_actual_draw_log_init();test_actual_draw_log_update(.0125f);
    CHECK(s_editor.frame_kpi_path[0]=='\0');CHECK(stats_calls==0);
    CHECK(rq_calls==0);CHECK(cull_calls==0);CHECK(s_editor.frame_kpi_index==0);
}
