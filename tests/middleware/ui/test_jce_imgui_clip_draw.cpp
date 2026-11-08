/* Complete actual first-party ImGui draw owner with typed CPU boundary ports.
 * Port PODs expose only fields the owner consumes; no upstream implementation,
 * GPU context, shader, presentation or hardware-performance proof is copied. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <jce/ui/jce_imgui_renderer.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_renderer.h>
#include "middleware/ui/jce_imgui_texture_upload_internal.h"
#ifndef JCE_IMGUI_CLIP_HEADER
#define JCE_IMGUI_CLIP_HEADER "middleware/ui/jce_imgui_clip_internal.h"
#endif
#include JCE_IMGUI_CLIP_HEADER
#include <array>
#include <cfloat>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <type_traits>
#include <vector>

struct ImVec2 { float x = 0, y = 0; };
struct ImVec4 { float x = 0, y = 0, z = 0, w = 0; };
enum { ImTextureStatus_OK, ImTextureStatus_WantCreate, ImTextureStatus_WantUpdates,
    ImTextureStatus_WantDestroy, ImTextureStatus_Destroyed, ImTextureFormat_RGBA32 };
static unsigned texture_id_reads;
struct ImTextureData { int Status = ImTextureStatus_OK; uint64_t id = 1; };
struct ImTextureRef {
    ImTextureData *_TexData = nullptr;
    uint64_t _TexID = 1;
    uint64_t GetTexID() const
    {
        ++texture_id_reads;
        if (_TexData) {
            CHECK((_TexData->Status == ImTextureStatus_OK ||
                _TexData->Status == ImTextureStatus_WantUpdates));
            return _TexData->id;
        }
        return _TexID;
    }
};
struct ImDrawList;
struct ImDrawCmd {
    void (*UserCallback)(const ImDrawList *, const ImDrawCmd *) = nullptr;
    ImVec4 ClipRect;
    ImTextureRef TexRef;
    unsigned VtxOffset = 0, IdxOffset = 0, ElemCount = 3;
};
struct ImDrawVert { ImVec2 pos, uv; uint32_t col = 0xffffffffu; };
using ImDrawIdx = uint16_t;
template<class T> struct Buffer {
    int Size = 0; T *Data = nullptr;
    const T &operator[](int i) const { return Data[i]; }
};
struct ImDrawList { Buffer<ImDrawVert> VtxBuffer; Buffer<ImDrawIdx> IdxBuffer; Buffer<ImDrawCmd> CmdBuffer; };
struct ImDrawData {
    std::vector<ImTextureData *> *Textures = nullptr;
    int TotalVtxCount = 0, CmdListsCount = 0;
    std::vector<ImDrawList *> CmdLists;
    ImVec2 DisplayPos;
};
static ImDrawData data;
static bool draw_data_available = true;
namespace ImGui { static ImDrawData *GetDrawData() { return draw_data_available ? &data : nullptr; } }
struct bgfx_program_handle_t { uint16_t idx = UINT16_MAX; };
struct bgfx_uniform_handle_t { uint16_t idx = UINT16_MAX; };
struct bgfx_frame_buffer_handle_t { uint16_t idx = UINT16_MAX; };
struct bgfx_texture_handle_t { uint16_t idx = UINT16_MAX; };
struct bgfx_vertex_layout_t { unsigned key = 0; };
struct bgfx_transient_vertex_buffer_t { uint8_t *data = nullptr; };
struct bgfx_transient_index_buffer_t { uint8_t *data = nullptr; };
struct bgfx_caps_t { bool originBottomLeft = false; };
#define BGFX_HANDLE_IS_VALID(value) ((value).idx != UINT16_MAX)
constexpr uint64_t BGFX_STATE_WRITE_RGB = 1, BGFX_STATE_WRITE_A = 2, BGFX_STATE_MSAA = 4;
constexpr uint64_t BGFX_STATE_BLEND_SRC_ALPHA = 8, BGFX_STATE_BLEND_INV_SRC_ALPHA = 16;
#define BGFX_STATE_BLEND_FUNC(source,dest) ((source) | (dest))
constexpr uint8_t BGFX_DISCARD_NONE = 0, BGFX_DISCARD_ALL = 255;
#include JCE_IMGUI_CLIP_STATE_OWNER

static unsigned allocations, scissors, textures, states, vertices, indices, callbacks;
static unsigned updates, rejected, clip_rejected, clip_counter_calls, readbacks;
static bool allocation_available = true;
static std::array<ImDrawVert, 256> vertex_memory;
static std::array<ImDrawIdx, 256> index_memory;
static JceImGuiClipRect last_clip;
static std::vector<uint16_t> views;
static uint64_t last_state;
static unsigned last_vertex_offset, last_index_offset, last_element_count;
static bool bgfx_alloc_transient_buffers(bgfx_transient_vertex_buffer_t *v,
    const bgfx_vertex_layout_t *, uint32_t nv, bgfx_transient_index_buffer_t *i,
    uint32_t ni, bool index32)
{
    ++allocations; CHECK_FALSE(index32);
    if (!allocation_available || nv > vertex_memory.size() || ni > index_memory.size()) return false;
    v->data = reinterpret_cast<uint8_t *>(vertex_memory.data());
    i->data = reinterpret_cast<uint8_t *>(index_memory.data());
    return true;
}
static uint16_t bgfx_set_scissor(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    ++scissors; last_clip = {x, y, w, h};
    CHECK(w > 0); CHECK(h > 0);
    CHECK(uint32_t(x) + w <= s_ctx.framebuffer_width);
    CHECK(uint32_t(y) + h <= s_ctx.framebuffer_height);
    return 0;
}
static void bgfx_set_texture(uint8_t stage, bgfx_uniform_handle_t uniform, bgfx_texture_handle_t texture, uint32_t sampler)
{ ++textures; CHECK(stage == 0); CHECK(uniform.idx == s_ctx.tex_uniform.idx); CHECK(texture.idx == 0); CHECK(sampler == UINT32_MAX); }
static void bgfx_set_state(uint64_t state, uint32_t rgba)
{ ++states; last_state = state; CHECK(rgba == 0); }
static void bgfx_set_transient_vertex_buffer(uint8_t stage, const bgfx_transient_vertex_buffer_t *, uint32_t start, uint32_t)
{ ++vertices; CHECK(stage == 0); last_vertex_offset = start; }
static void bgfx_set_transient_index_buffer(const bgfx_transient_index_buffer_t *, uint32_t start, uint32_t count)
{ ++indices; last_index_offset = start; last_element_count = count; }
static void bgfx_submit(uint16_t view, bgfx_program_handle_t program, uint32_t depth, uint8_t discard)
{ views.push_back(view); CHECK(program.idx == s_ctx.program.idx); CHECK(depth == 0); CHECK(discard == (view == s_ctx.view_id && s_rec.active ? BGFX_DISCARD_NONE : BGFX_DISCARD_ALL)); }
static const bgfx_caps_t *bgfx_get_caps() { static const bgfx_caps_t caps; return &caps; }
static void update_imgui_texture(ImTextureData *) { ++updates; }
static void trace_counter_port(JceTraceCategory category, const char *name, int64_t value)
{
    CHECK(category == JCE_TRACE_CATEGORY_RENDER);
    CHECK(value > 0);
    if (std::strcmp(name, "ui.clip.draw.rejected") == 0) {
        ++clip_counter_calls;
        clip_rejected += unsigned(value);
    } else {
        CHECK(std::strcmp(name, "ui.atlas.draw.rejected") == 0);
        rejected += unsigned(value);
    }
}
static uint64_t ticks_port() { return 0; }
static bool sink_port(uint16_t, uint16_t, uint16_t, uint16_t, int) { ++readbacks; return true; }
static_assert(std::is_same_v<decltype(&trace_counter_port), decltype(&jce_trace_counter)>);
static_assert(std::is_same_v<decltype(&ticks_port), decltype(&jce_time_ticks_ms)>);
static_assert(std::is_same_v<decltype(&sink_port), decltype(&jce_renderer_readback_capture_submit_sink)>);
#define jce_trace_counter trace_counter_port
#define jce_time_ticks_ms ticks_port
#define jce_renderer_readback_capture_submit_sink sink_port
#include JCE_IMGUI_CLIP_DRAW_OWNER
#undef jce_trace_counter
#undef jce_time_ticks_ms
#undef jce_renderer_readback_capture_submit_sink
static void actual_setup_extent(uint16_t width, uint16_t height)
{
#include JCE_IMGUI_CLIP_EXTENT_OWNER
}
static void callback_port(const ImDrawList *, const ImDrawCmd *) { ++callbacks; }

struct Fixture {
    std::array<ImDrawVert, 3> v{};
    std::array<ImDrawIdx, 3> i{0,1,2};
    std::vector<ImDrawCmd> commands;
    ImDrawList list;
    Fixture()
    {
        allocations = scissors = textures = states = vertices = indices = callbacks = 0;
        updates = rejected = clip_rejected = clip_counter_calls = readbacks = 0;
        allocation_available = draw_data_available = true; texture_id_reads = 0;
        last_clip = {}; views.clear(); last_state = 0;
        last_vertex_offset = last_index_offset = last_element_count = 0;
        s_ctx = {}; s_ctx.initialized = true; s_ctx.view_id = 250;
        s_ctx.program.idx = 42; s_ctx.tex_uniform.idx = 7;
        s_rec.active = false; s_rec.fb.idx = UINT16_MAX;
        data = {}; data.TotalVtxCount = 3; data.CmdListsCount = 1;
        list.VtxBuffer = {3, v.data()}; list.IdxBuffer = {3, i.data()};
        data.CmdLists.push_back(&list);
        actual_setup_extent(640, 360);
    }
    void prepare(ImVec4 rect)
    {
        ImDrawCmd cmd{}; cmd.ClipRect = rect;
        commands.assign(1, cmd); list.CmdBuffer = {1, commands.data()};
    }
    void draw(ImVec4 rect)
    {
        prepare(rect);
        jce_imgui_renderer_draw();
    }
    JceImGuiDrawObservation observe(ImVec4 rect, uint16_t watched = 0)
    {
        prepare(rect);
        JceImGuiDrawObservation observation{};
        REQUIRE(jce_imgui_renderer_draw_observed(watched, &observation, sizeof observation));
        CHECK(observation.struct_size == sizeof observation);
        return observation;
    }
    void expect_no_draw()
    {
        CHECK(scissors == 0); CHECK(textures == 0); CHECK(states == 0);
        CHECK(vertices == 0); CHECK(indices == 0); CHECK(views.empty());
    }
    void expect_clip(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
    {
        CHECK(scissors == 1); CHECK(textures == 1); CHECK(states == 1);
        CHECK(vertices == 1); CHECK(indices == 1); REQUIRE(views.size() == 1);
        CHECK(views[0] == 250); CHECK(last_clip.x == x); CHECK(last_clip.y == y);
        CHECK(last_clip.width == w); CHECK(last_clip.height == h);
        CHECK(last_state == (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA |
            BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA)));
        CHECK(last_vertex_offset == 0); CHECK(last_index_offset == 0); CHECK(last_element_count == 3);
    }
};
TEST_CASE_FIXTURE(Fixture, "actual valid draw keeps clip rounding state and geometry offsets")
{
    draw({10.9f, 20.9f, 30.7f, 40.7f}); expect_clip(10, 20, 20, 20);
    CHECK(allocations == 1);
    CHECK(std::memcmp(vertex_memory.data(), v.data(), sizeof v) == 0);
    CHECK(std::memcmp(index_memory.data(), i.data(), sizeof i) == 0);
    CHECK(clip_rejected == 0); CHECK(clip_counter_calls == 0);
}
TEST_CASE_FIXTURE(Fixture, "actual partially negative clip intersects without wrap")
{ draw({-10, -20, 20, 30}); expect_clip(0, 0, 20, 30); }
TEST_CASE_FIXTURE(Fixture, "actual framebuffer right and bottom bounds clamp valid geometry")
{ draw({620, 340, 700, 500}); expect_clip(620, 340, 20, 20); }
TEST_CASE_FIXTURE(Fixture, "actual wholly outside framebuffer skips all draw state")
{
    draw({700, 10, 900, 20}); expect_no_draw();
    draw({10, 370, 30, 500}); expect_no_draw();
    draw({-20, -30, -10, -5}); expect_no_draw();
    CHECK(clip_rejected == 3); CHECK(clip_counter_calls == 3);
}
TEST_CASE_FIXTURE(Fixture, "actual touching or empty framebuffer intersections skip submit")
{
    draw({640, 0, 900, 50}); expect_no_draw();
    draw({0, 360, 20, 400}); expect_no_draw();
    draw({10, 10, 10, 30}); expect_no_draw();
    draw({10, 10, 9, 30}); expect_no_draw();
}
TEST_CASE_FIXTURE(Fixture, "actual nonfinite clip and display origin refuse narrowing")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (unsigned field = 0; field < 4; ++field) {
        std::array<float,4> values{1,2,3,4};
        for (float invalid : {nan, inf, -inf}) {
            values[field] = invalid;
            draw({values[0],values[1],values[2],values[3]}); expect_no_draw();
        }
    }
    data.DisplayPos = {nan,0}; draw({1,2,3,4}); expect_no_draw();
    data.DisplayPos = {0,inf}; draw({1,2,3,4}); expect_no_draw();
}
TEST_CASE_FIXTURE(Fixture, "actual DisplayPos is removed once using the existing pixel convention")
{ data.DisplayPos = {100,50}; draw({110.9f,70.9f,130.7f,90.7f}); expect_clip(10,20,20,20); }
TEST_CASE_FIXTURE(Fixture, "actual finite subtraction rounding remains at the old integer edge")
{ data.DisplayPos = {1.0e-9f,1.0e-9f}; draw({1,1,4,4}); expect_clip(1,1,3,3); }
TEST_CASE_FIXTURE(Fixture, "actual huge finite clip clamps without float subtraction overflow")
{ data.DisplayPos = {-FLT_MAX,-FLT_MAX}; draw({-FLT_MAX,-FLT_MAX,FLT_MAX,FLT_MAX}); expect_clip(0,0,640,360); }
TEST_CASE_FIXTURE(Fixture, "actual zero and resized extent use current setup owner not stale display size")
{
    actual_setup_extent(0, 360); draw({1,1,10,10}); expect_no_draw();
    actual_setup_extent(640, 0); draw({1,1,10,10}); expect_no_draw();
    actual_setup_extent(20, 10); draw({0,0,40,20}); expect_clip(0,0,20,10);
}
TEST_CASE_FIXTURE(Fixture, "actual upper uint16 edge stays representable")
{
    actual_setup_extent(UINT16_MAX, UINT16_MAX);
    draw({0,0,65536,65536}); expect_clip(0,0,UINT16_MAX,UINT16_MAX);
    JceImGuiClipRect clip;
    CHECK_FALSE(jce_imgui_clip_intersect(0,0,1,1,0,0,65536,10,&clip));
    CHECK_FALSE(jce_imgui_clip_intersect(0,0,1,1,0,0,10,65536,&clip));
    CHECK_FALSE(jce_imgui_clip_intersect(0,0,1,1,0,0,10,10,nullptr));
}
TEST_CASE_FIXTURE(Fixture, "actual integer-empty fractional clip does not consume draw state")
{ draw({10.1f,10.1f,10.9f,10.9f}); expect_no_draw(); }
TEST_CASE_FIXTURE(Fixture, "actual callback is retained but clipped normal command does not submit")
{
    ImDrawCmd callback{}; callback.UserCallback = callback_port;
    ImDrawCmd clipped{}; clipped.ClipRect = {700,0,900,100};
    commands = {callback,clipped}; list.CmdBuffer = {2,commands.data()};
    jce_imgui_renderer_draw(); CHECK(callbacks == 1); expect_no_draw();
    CHECK(clip_rejected == 1); CHECK(clip_counter_calls == 1);
}
TEST_CASE_FIXTURE(Fixture, "actual texture maintenance precedes zero-vertex return")
{
    ImTextureData request{ImTextureStatus_WantCreate};
    std::vector<ImTextureData *> requests{&request}; data.Textures = &requests;
    data.TotalVtxCount = 0; jce_imgui_renderer_draw();
    CHECK(updates == 1); CHECK(allocations == 0); expect_no_draw();
}
TEST_CASE_FIXTURE(Fixture, "actual transient allocation refusal preserves no-partial-copy guard")
{ allocation_available = false; draw({0,0,20,20}); CHECK(allocations == 1); expect_no_draw(); }
TEST_CASE_FIXTURE(Fixture, "actual rejected texture never reaches state or submit")
{
    ImDrawCmd cmd{}; cmd.ClipRect = {0,0,20,20}; cmd.TexRef._TexID = 0;
    commands = {cmd}; list.CmdBuffer = {1,commands.data()};
    jce_imgui_renderer_draw(); CHECK(scissors == 1); CHECK(textures == 0);
    CHECK(states == 0); CHECK(views.empty()); CHECK(rejected == 1);
}
TEST_CASE_FIXTURE(Fixture, "actual recording mirror retains valid double submit and rejects outside")
{
    s_rec.active = true; s_rec.fb.idx = 0; draw({10,20,30,40});
    REQUIRE(views.size() == 2); CHECK(views[0] == 250); CHECK(views[1] == 251);
    views.clear(); scissors = textures = states = vertices = indices = 0;
    draw({700,0,900,40}); expect_no_draw(); CHECK(readbacks == 0);
}
TEST_CASE_FIXTURE(Fixture, "actual uninitialized draw and setup preserve prior cached extent")
{
    s_ctx.initialized = false; actual_setup_extent(20,10);
    CHECK(s_ctx.framebuffer_width == 640); CHECK(s_ctx.framebuffer_height == 360);
    draw({1,1,2,2}); expect_no_draw(); CHECK(allocations == 0);
}


TEST_CASE_FIXTURE(Fixture, "observation counts actual matching queue and main calls without changing draw")
{
    const auto observed = observe({10,20,30,40});
    CHECK(observed.flags == (JCE_IMGUI_DRAW_QUEUE_OBSERVED | JCE_IMGUI_DRAW_SUBMISSIONS_OBSERVED));
    CHECK(observed.queued_commands == 1); CHECK(observed.main_submits == 1);
    CHECK(observed.recording_submits == 0); expect_clip(10,20,20,20);
    CHECK(texture_id_reads == 2); CHECK(allocations == 1);
}
TEST_CASE_FIXTURE(Fixture, "default draw does not inspect the command queue again")
{
    draw({10,20,30,40}); expect_clip(10,20,20,20);
    CHECK(texture_id_reads == 1);
}
TEST_CASE_FIXTURE(Fixture, "observation valid zero for unmatched watched handle does not suppress real draw")
{
    const auto observed = observe({10,20,30,40}, 1);
    CHECK(observed.flags == 3); CHECK(observed.queued_commands == 0);
    CHECK(observed.main_submits == 0); CHECK(observed.recording_submits == 0);
    expect_clip(10,20,20,20);
}
TEST_CASE_FIXTURE(Fixture, "observation distinguishes clipped queue from accepted submits")
{
    const auto observed = observe({700,0,900,40}); expect_no_draw();
    CHECK(observed.flags == 3); CHECK(observed.queued_commands == 1);
    CHECK(observed.main_submits == 0); CHECK(observed.recording_submits == 0);
}
TEST_CASE_FIXTURE(Fixture, "observation preserves queued evidence when drawable dimensions are zero")
{
    actual_setup_extent(0,0);
    const auto observed = observe({10,20,30,40}); expect_no_draw();
    CHECK(observed.flags == 3); CHECK(observed.queued_commands == 1);
    CHECK(observed.main_submits == 0); CHECK(observed.recording_submits == 0);
}
TEST_CASE_FIXTURE(Fixture, "observation scans all queued lists before a transient refusal")
{
    ImDrawCmd command{}; command.ClipRect = {0,0,20,20};
    std::array<ImDrawCmd,2> second_commands{command,command};
    ImDrawList second{}; second.CmdBuffer = {2,second_commands.data()};
    second.VtxBuffer = list.VtxBuffer; second.IdxBuffer = list.IdxBuffer;
    prepare({10,20,30,40}); data.CmdLists.push_back(&second); data.CmdListsCount = 2;
    allocation_available = false;
    JceImGuiDrawObservation observed{};
    REQUIRE(jce_imgui_renderer_draw_observed(0,&observed,sizeof observed)); expect_no_draw();
    CHECK(observed.flags == 3); CHECK(observed.queued_commands == 3);
    CHECK(observed.main_submits == 0); CHECK(observed.recording_submits == 0);
    CHECK(allocations == 1); CHECK(texture_id_reads == 3);
}
TEST_CASE_FIXTURE(Fixture, "observation records final queue even when zero vertices prevents draw")
{
    data.TotalVtxCount = 0;
    ImTextureData request{ImTextureStatus_WantCreate};
    std::vector<ImTextureData *> requests{&request}; data.Textures = &requests;
    const auto observed = observe({10,20,30,40}); expect_no_draw();
    CHECK(observed.flags == 3); CHECK(observed.queued_commands == 1);
    CHECK(observed.main_submits == 0); CHECK(allocations == 0); CHECK(updates == 1);
}
TEST_CASE_FIXTURE(Fixture, "observation excludes callbacks and empty commands while preserving their behavior")
{
    ImDrawCmd callback{}; callback.UserCallback = callback_port; callback.TexRef._TexID = UINT64_MAX;
    ImDrawCmd empty{}; empty.ElemCount = 0; empty.ClipRect = {0,0,20,20};
    commands = {callback,empty}; list.CmdBuffer = {2,commands.data()};
    JceImGuiDrawObservation observed{};
    REQUIRE(jce_imgui_renderer_draw_observed(0,&observed,sizeof observed));
    CHECK(observed.flags == 3); CHECK(observed.queued_commands == 0);
    CHECK(observed.main_submits == 0); CHECK(observed.recording_submits == 0);
    CHECK(callbacks == 1); CHECK(views.size() == 1); CHECK(last_element_count == 0);
    CHECK(texture_id_reads == 1);
}
TEST_CASE_FIXTURE(Fixture, "observation never resolves pending font texture IDs and rejects oversized IDs")
{
    ImTextureData pending{ImTextureStatus_WantCreate};
    prepare({10,20,30,40}); commands[0].TexRef._TexData = &pending;
    JceImGuiDrawObservation observed{};
    for (int status : {ImTextureStatus_WantCreate, ImTextureStatus_WantDestroy, ImTextureStatus_Destroyed}) {
        pending.Status = status;
        REQUIRE(jce_imgui_renderer_draw_observed(0,&observed,sizeof observed));
        CHECK(observed.flags == 3); CHECK(observed.queued_commands == 0);
        CHECK(observed.main_submits == 0); CHECK(texture_id_reads == 0); CHECK(views.empty());
    }
    pending.Status = ImTextureStatus_WantUpdates;
    REQUIRE(jce_imgui_renderer_draw_observed(0,&observed,sizeof observed));
    CHECK(observed.queued_commands == 1); CHECK(observed.main_submits == 1);
    CHECK(texture_id_reads == 2);
    commands[0].TexRef._TexData = nullptr; commands[0].TexRef._TexID = UINT64_C(65537);
    REQUIRE(jce_imgui_renderer_draw_observed(0,&observed,sizeof observed));
    CHECK(observed.queued_commands == 0); CHECK(observed.main_submits == 0);
}
TEST_CASE_FIXTURE(Fixture, "observation separates actual main and recording submission calls")
{
    s_rec.active = true; s_rec.fb.idx = 0;
    const auto observed = observe({10,20,30,40});
    CHECK(observed.flags == 3); CHECK(observed.queued_commands == 1);
    CHECK(observed.main_submits == 1); CHECK(observed.recording_submits == 1);
    REQUIRE(views.size() == 2); CHECK(views[0] == 250); CHECK(views[1] == 251);
}
TEST_CASE_FIXTURE(Fixture, "unavailable init or draw data writes absence instead of stale queue")
{
    s_ctx.initialized = false;
    const auto uninitialized = observe({10,20,30,40});
    CHECK(uninitialized.flags == 0); CHECK(uninitialized.queued_commands == 0);
    CHECK(uninitialized.main_submits == 0); CHECK(uninitialized.recording_submits == 0);
    s_ctx.initialized = true; draw_data_available = false;
    const auto absent = observe({10,20,30,40});
    CHECK(absent.flags == 0); CHECK(absent.queued_commands == 0); CHECK(absent.main_submits == 0);
    CHECK(allocations == 0); CHECK(texture_id_reads == 0); expect_no_draw();
}
TEST_CASE_FIXTURE(Fixture, "rejected sized requests preserve every byte while drawing once")
{
    struct Guarded { JceImGuiDrawObservation value; uint8_t tail[12]; } out;
    std::memset(&out,0xa5,sizeof out); const auto before = out;
    prepare({10,20,30,40});
    CHECK_FALSE(jce_imgui_renderer_draw_observed(0,&out.value,sizeof(out.value)-1));
    CHECK(std::memcmp(&out,&before,sizeof out) == 0); REQUIRE(views.size() == 1);
    CHECK_FALSE(jce_imgui_renderer_draw_observed(UINT16_MAX,&out.value,sizeof out));
    CHECK(std::memcmp(&out,&before,sizeof out) == 0); REQUIRE(views.size() == 2);
    CHECK_FALSE(jce_imgui_renderer_draw_observed(0,nullptr,sizeof out));
    REQUIRE(views.size() == 3); CHECK(texture_id_reads == 3);
    REQUIRE(jce_imgui_renderer_draw_observed(0,&out.value,sizeof out));
    CHECK(out.value.struct_size == sizeof out.value); CHECK(out.value.main_submits == 1);
    CHECK(std::memcmp(out.tail,before.tail,sizeof out.tail) == 0); REQUIRE(views.size() == 4);
}
TEST_CASE_FIXTURE(Fixture, "observation counter overflow refuses publication without wrapping")
{
    ImGuiDrawObservationState observed{}; observed.value.main_submits = UINT32_MAX;
    imgui_observation_increment(&observed,&observed.value.main_submits);
    CHECK(observed.overflow); CHECK(observed.value.main_submits == UINT32_MAX);
}
