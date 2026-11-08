/* Actual private override owner + public pipeline and Archive/PAK/VFS.
 * Hardware caps are controlled; no Editor UI, rendering or GPU is started. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "io/jce_editor_bundle_render_pipeline.h"
#include "core/jce_editor_cpu_scope.h"
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_pak_loader.h>
#include <cstring>
#include <string>
#include <vector>

static JceGpuTier tier = JCE_GPU_TIER_HIGH;
extern "C" {
JceRendererBackend jce_renderer_get_active_backend(void) { return JCE_BACKEND_D3D11; }
JceGpuTier jce_renderer_get_tier(void) { return tier; }
uint32_t jce_renderer_get_caps(void) { return JCE_CAP_TEXTURE_FLOAT | JCE_CAP_FRAMEBUFFER_FLOAT; }
JceRenderRecommendation jce_renderer_get_recommendation(void)
{ JceRenderRecommendation r{}; r.has_discrete_gpu = true; return r; }
const char *jce_gpu_tier_name(JceGpuTier) { return "controlled"; }
}
static const void *JCE_CALL find_pak(const JcePakArchive *p, const char *k) { return jce_pak_find(p,k); }
static uint64_t JCE_CALL size_pak(const void *a) { return static_cast<const JcePakAsset *>(a)->original_size; }
static size_t JCE_CALL decode_pak(const void *a, void *b, size_t n)
{ return jce_pak_decompress(static_cast<const JcePakAsset *>(a),b,n); }
static const JceFsPakProvider provider{find_pak,size_pak,decode_pak};
struct Fixture {
    const JceFsPakProvider *previous = jce_fs_get_pak_provider();
    JceFileSystem *fs = nullptr; JcePakArchive *pak = nullptr; void *blob = nullptr;
    JceEditorBundlePipeline state;
    explicit Fixture(const std::string &bytes, bool present = true)
    {
        tier = JCE_GPU_TIER_HIGH; jce_fs_set_pak_provider(&provider);
        JceArchiveWriter *w = jce_archive_writer_create(nullptr); REQUIRE(w);
        REQUIRE(jce_archive_writer_add(w, present ? "settings/render_pipeline.rp.json" : "unrelated",
            bytes.data(), bytes.size()));
        size_t size = 0; REQUIRE(jce_archive_writer_finish(w,&blob,&size)); jce_archive_writer_destroy(w);
        pak = jce_pak_open(blob,size); REQUIRE(pak); fs = jce_fs_create(); REQUIRE(fs);
        REQUIRE(jce_fs_mount_pak(fs,pak));
        JceRenderPipelineDesc project; jce_render_pipeline_preset_high(&project);
        project.render_scale = 0.9f; jce_render_pipeline_apply(&project);
    }
    ~Fixture()
    {
        jce_editor_bundle_pipeline_close(&state); jce_fs_destroy(fs); jce_pak_close(pak);
        jce_free(blob); jce_fs_set_pak_provider(previous);
    }
};
static JceRenderPipelineDesc request() { JceRenderPipelineDesc d; jce_render_pipeline_get_requested(&d); return d; }
TEST_CASE("Bundle applies before use and close restores project requested intent across tiers")
{
    Fixture f(R"({"enable_volumetric_fog":false,"render_scale":0.65})");
    tier = JCE_GPU_TIER_LOW; jce_render_pipeline_notify_tier_changed();
    jce_render_pipeline_set_knob("render_scale",0.8f);
    CHECK(request().render_scale == doctest::Approx(0.9)); // pending excluded
    REQUIRE(jce_editor_bundle_pipeline_prepare(&f.state,f.fs));
    CHECK(request().render_scale == doctest::Approx(0.9)); // prepare is read-only
    REQUIRE(jce_editor_bundle_pipeline_apply(&f.state));
    CHECK(request().render_scale == doctest::Approx(0.65));
    CHECK_FALSE(request().enable_volumetric_fog);
    CHECK_FALSE(jce_editor_bundle_pipeline_apply(&f.state));
    CHECK_FALSE(jce_editor_bundle_pipeline_prepare(&f.state,f.fs));
    jce_editor_bundle_pipeline_close(&f.state);
    CHECK(request().render_scale == doctest::Approx(0.8));
    CHECK(request().post_quality == JCE_RP_QUALITY_HIGH);
    tier = JCE_GPU_TIER_HIGH; jce_render_pipeline_notify_tier_changed();
    JceRenderPipelineDesc active; jce_render_pipeline_get(&active);
    CHECK(active.post_quality == JCE_RP_QUALITY_HIGH);
    jce_editor_bundle_pipeline_close(&f.state);
    CHECK(request().post_quality == JCE_RP_QUALITY_HIGH);
}
TEST_CASE("Only archive absence selects the explicit current tier fallback")
{
    Fixture f("unrelated",false); tier = JCE_GPU_TIER_LOW;
    REQUIRE(jce_editor_bundle_pipeline_prepare(&f.state,f.fs)); CHECK(f.state.used_tier);
    REQUIRE(jce_editor_bundle_pipeline_apply(&f.state));
    CHECK(request().post_quality == JCE_RP_QUALITY_LOW);
    CHECK(request().render_scale == doctest::Approx(1.0));
    jce_editor_bundle_pipeline_close(&f.state);
    CHECK(request().post_quality == JCE_RP_QUALITY_HIGH);
    CHECK(request().render_scale == doctest::Approx(0.9));
}
TEST_CASE("Present malformed or oversized RP refuses and clears loader output")
{
    std::string bytes;
    SUBCASE("syntax") { bytes = "{"; }
    SUBCASE("array") { bytes = "[]"; }
    SUBCASE("scalar") { bytes = "true"; }
    SUBCASE("trailing junk") { bytes = "{}trailing"; }
    SUBCASE("limit") { bytes.assign((1u<<20)+1u,' '); }
    Fixture f(bytes); const JceRenderPipelineDesc before = request();
    CHECK_FALSE(jce_editor_bundle_pipeline_prepare(&f.state,f.fs));
    CHECK_FALSE(f.state.ready); CHECK_FALSE(f.state.used_tier);
    CHECK_FALSE(jce_editor_bundle_pipeline_apply(&f.state));
    CHECK(request().render_scale == before.render_scale);
    JceRenderPipelineDesc output; std::memset(&output,0x7f,sizeof output);
    CHECK_FALSE(jce_render_pipeline_load_fs(f.fs,"settings/render_pipeline.rp.json",&output));
    const JceRenderPipelineDesc zero{}; CHECK(std::memcmp(&output,&zero,sizeof output)==0);
}
TEST_CASE("Null and missing loads clear outputs without applying")
{
    Fixture f("unrelated",false); JceRenderPipelineDesc output, zero{};
    std::memset(&output,0x7f,sizeof output);
    CHECK_FALSE(jce_render_pipeline_load_fs(nullptr,"key",&output));
    CHECK(std::memcmp(&output,&zero,sizeof output)==0);
    CHECK_FALSE(jce_render_pipeline_load_fs(f.fs,nullptr,&output));
    CHECK_FALSE(jce_render_pipeline_load_fs(f.fs,"key",nullptr));
    CHECK_FALSE(jce_editor_bundle_pipeline_prepare(nullptr,f.fs));
    CHECK_FALSE(jce_editor_bundle_pipeline_prepare(&f.state,nullptr));
    CHECK_FALSE(jce_editor_bundle_pipeline_apply(nullptr));
    jce_editor_bundle_pipeline_close(nullptr);
    CHECK(request().render_scale == doctest::Approx(0.9));
}

namespace {
struct TraceCapture {
    TraceCapture() {
        jce_trace_set_enabled(false);jce_trace_reset();
        jce_trace_set_enabled(true);jce_trace_thread_register("bundle-rp-owner-test");
    }
    ~TraceCapture() {
        jce_trace_thread_unregister();jce_trace_set_enabled(false);jce_trace_reset();
    }
    std::vector<JceTraceEvent> read() const {
        JceTraceCursor cursor;jce_trace_cursor_init(&cursor);JceTraceEvent events[512];
        const auto count=jce_trace_read(&cursor,events,512);
        CHECK(count<512);CHECK(cursor.lost_events==0);
        JceTraceStats stats{};jce_trace_get_stats(&stats);CHECK(stats.active_work_items==0);
        return {events,events+count};
    }
};
struct ObservedScope {
    uint64_t id=0,parent=0;
    uint32_t begin=0,end=0;
    JceTraceTaskState state=JCE_TRACE_TASK_FAILED;
};
static std::vector<ObservedScope> scopes(const std::vector<JceTraceEvent> &events,
                                        const char *name,unsigned expected)
{
    std::vector<ObservedScope> result;
    unsigned named_begins=0,named_ends=0;
    for(const auto &event:events) {
        if(std::strcmp(event.name,name))continue;
        if(event.type==JCE_TRACE_EVENT_TASK_BEGIN)++named_begins;
        if(event.type==JCE_TRACE_EVENT_TASK_END)++named_ends;
        if(event.type!=JCE_TRACE_EVENT_TASK_SUBMIT)continue;
        ObservedScope scope;scope.id=event.id;scope.parent=event.parent_id;
        CHECK(event.value_u32[0]==JCE_TRACE_TASK_SERVICE);
        unsigned begins=0,ends=0;
        for(const auto &part:events) {
            if(part.id!=scope.id || std::strcmp(part.name,name))continue;
            if(part.type==JCE_TRACE_EVENT_TASK_BEGIN) {
                ++begins;scope.begin=part.sequence;CHECK(part.parent_id==scope.id);
                CHECK(part.thread_id==event.thread_id);
            }
            if(part.type==JCE_TRACE_EVENT_TASK_END) {
                ++ends;scope.end=part.sequence;CHECK(part.parent_id==scope.id);
                CHECK(part.thread_id==event.thread_id);
                scope.state=static_cast<JceTraceTaskState>(part.value_u32[0]);
            }
        }
        CHECK(begins==1);CHECK(ends==1);CHECK(event.sequence<scope.begin);CHECK(scope.begin<scope.end);
        result.push_back(scope);
    }
    REQUIRE(result.size()==expected);CHECK(named_begins==expected);CHECK(named_ends==expected);
    return result;
}
static void counter(const std::vector<JceTraceEvent> &events,const char *name,
                    const ObservedScope &scope,uint64_t value)
{
    unsigned count=0;
    for(const auto &event:events) {
        if(event.type!=JCE_TRACE_EVENT_COUNTER || std::strcmp(event.name,name) || event.id!=scope.id)continue;
        ++count;CHECK(event.parent_id==scope.id);CHECK(event.category==JCE_TRACE_CATEGORY_CORE);
        CHECK(event.value_u64==value);CHECK(scope.begin<event.sequence);CHECK(event.sequence<scope.end);
    }
    CHECK(count==1);
}
static void child(const ObservedScope &nested,const ObservedScope &owner)
{
    CHECK(nested.parent==owner.id);CHECK(owner.begin<nested.begin);CHECK(nested.end<owner.end);
}
}
TEST_CASE("Bundle owner scopes contain real apply and once-only restore publications")
{
    bool absent=false;
    SUBCASE("authored pipeline") {}
    SUBCASE("confirmed absence fallback") { absent=true; }
    Fixture f(absent?"unrelated":R"({"enable_volumetric_fog":false,"render_scale":0.65})",!absent);
    TraceCapture capture;uint64_t parent=0;
    {
        JceEditorCpuScope caller("bundle-pipeline.caller");parent=jce_trace_task_current_id();
        REQUIRE(jce_editor_bundle_pipeline_prepare(&f.state,f.fs));
        CHECK(jce_trace_task_current_id()==parent);CHECK(request().render_scale==doctest::Approx(0.9));
        REQUIRE(jce_editor_bundle_pipeline_apply(&f.state));CHECK(jce_trace_task_current_id()==parent);
        jce_editor_bundle_pipeline_close(&f.state);CHECK(jce_trace_task_current_id()==parent);
        jce_editor_bundle_pipeline_close(&f.state);jce_editor_bundle_pipeline_close(nullptr);
        CHECK(jce_trace_task_current_id()==parent);CHECK(request().render_scale==doctest::Approx(0.9));
    }
    CHECK(jce_trace_task_current_id()==0);
    const auto events=capture.read();
    const auto prepared=scopes(events,"editor.bundle_pipeline.prepare",1);
    const auto applied=scopes(events,"editor.bundle_pipeline.apply",1);
    const auto restored=scopes(events,"editor.bundle_pipeline.restore",1);
    const auto published=scopes(events,"render_pipeline.publish",2);
    CHECK(prepared[0].parent==parent);CHECK(applied[0].parent==parent);CHECK(restored[0].parent==parent);
    CHECK(prepared[0].state==JCE_TRACE_TASK_SUCCEEDED);
    CHECK(applied[0].state==JCE_TRACE_TASK_SUCCEEDED);CHECK(restored[0].state==JCE_TRACE_TASK_SUCCEEDED);
    child(published[0],applied[0]);child(published[1],restored[0]);
    counter(events,"bundle_rp.tier_fallback",prepared[0],absent?1:0);
    counter(events,"bundle_rp.active",applied[0],1);counter(events,"bundle_rp.active",restored[0],0);
    counter(events,"rp.scale_milli",published[0],absent?1000:650);
    counter(events,"rp.scale_milli",published[1],900);
}
TEST_CASE("Refused Bundle preparation and apply are failed scopes without policy publication")
{
    std::string bytes="{";bool null_fs=false,null_state=false;
    SUBCASE("malformed present") {}
    SUBCASE("over limit present") { bytes.assign((1u<<20)+1u,' '); }
    SUBCASE("missing filesystem") { null_fs=true; }
    SUBCASE("missing owner") { null_state=true; }
    Fixture f(bytes);const auto before=request();TraceCapture capture;
    CHECK_FALSE(jce_editor_bundle_pipeline_prepare(null_state?nullptr:&f.state,null_fs?nullptr:f.fs));
    CHECK_FALSE(jce_editor_bundle_pipeline_apply(null_state?nullptr:&f.state));
    jce_editor_bundle_pipeline_close(&f.state);
    CHECK(request().render_scale==before.render_scale);CHECK(request().post_quality==before.post_quality);
    CHECK_FALSE(f.state.active);CHECK_FALSE(f.state.ready);
    const auto events=capture.read();
    CHECK(scopes(events,"editor.bundle_pipeline.prepare",1)[0].state==JCE_TRACE_TASK_FAILED);
    CHECK(scopes(events,"editor.bundle_pipeline.apply",1)[0].state==JCE_TRACE_TASK_FAILED);
    (void)scopes(events,"editor.bundle_pipeline.restore",0);
    (void)scopes(events,"render_pipeline.publish",0);
    CHECK(jce_trace_task_current_id()==0);
}
TEST_CASE("Active Bundle admission refusal preserves its policy and reports failed attempts")
{
    Fixture f(R"({"render_scale":0.65})");
    REQUIRE(jce_editor_bundle_pipeline_prepare(&f.state,f.fs));
    REQUIRE(jce_editor_bundle_pipeline_apply(&f.state));
    const auto before=request();TraceCapture capture;
    CHECK_FALSE(jce_editor_bundle_pipeline_prepare(&f.state,f.fs));
    CHECK_FALSE(jce_editor_bundle_pipeline_apply(&f.state));
    CHECK(f.state.active);CHECK(f.state.ready);CHECK(request().render_scale==before.render_scale);
    const auto events=capture.read();
    CHECK(scopes(events,"editor.bundle_pipeline.prepare",1)[0].state==JCE_TRACE_TASK_FAILED);
    CHECK(scopes(events,"editor.bundle_pipeline.apply",1)[0].state==JCE_TRACE_TASK_FAILED);
    (void)scopes(events,"editor.bundle_pipeline.restore",0);
    (void)scopes(events,"render_pipeline.publish",0);
}
