/* Actual grid-caption owner and filtered-copy wiring with real ImGui CPU text
 * metrics/layout. No native window, renderer, GPU or filesystem scan is run. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_imgui_context.h"
#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include JCE_TEST_ASSET_CAPTION_OWNER
#include JCE_TEST_ASSET_CAPTION_WIRING

AssetBrowserState s_assets{};

namespace {
struct CaptionFrame {
    JceTestImGuiContext imgui;
    void begin()
    {
        imgui.begin();
        ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_Always);
        ImGui::Begin("asset-caption", nullptr, ImGuiWindowFlags_NoSavedSettings);
    }
    void end() { ImGui::End(); imgui.end(); }
};

struct CountedHeight {
    size_t calls = 0;
    float operator()(const char *text, float width)
    {
        ++calls;
        return ImGui::CalcTextSize(text, nullptr, false, width).y;
    }
};

/* Independent pre-cache owner reference: all names were unsplit, including
 * their extensions. Preserve the original snprintf byte-prefix policy. */
std::string original_caption(const std::string &name, float width, float height)
{
    if (ImGui::CalcTextSize(name.c_str(), nullptr, false, width).y <= height)
        return name;
    char trunc[256];
    const char *src = name.c_str();
    int cut = (int)strlen(src) - 1;
    for (; cut > 0; --cut) {
        snprintf(trunc, sizeof(trunc), "%.*s...", cut, src);
        if (ImGui::CalcTextSize(trunc, nullptr, false, width).y <= height)
            break;
    }
    if (cut <= 0) snprintf(trunc, sizeof(trunc), "...");
    return trunc;
}

void original_draw(const std::string &name, bool selected, float width,
                   ImVec4 selected_color)
{
    float max_height = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec4 color = selected ? selected_color : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    const std::string caption = selected ? name : original_caption(name, width, max_height);
    ImGui::TextWrapped("%s", caption.c_str());
    ImGui::PopStyleColor();
    if (!selected) {
        float used = ImGui::GetCursorScreenPos().y - start.y;
        if (used < max_height) ImGui::Dummy(ImVec2(0, max_height - used));
    }
    ImGui::PopTextWrapPos();
}

struct DrawResult {
    ImVec2 cursor, item_min, item_max;
    std::vector<ImDrawVert> vertices;
};

DrawResult draw(CaptionFrame &frame, AssetGridCaptionCache &cache,
               const std::string &name, bool selected, bool old, float width)
{
    frame.begin();
    ImDrawList *list = ImGui::GetWindowDrawList();
    int first = list->VtxBuffer.Size;
    const ImVec4 selected_color(0.9f, 0.85f, 0.7f, 1.0f);
    if (old) original_draw(name, selected, width, selected_color);
    else draw_asset_grid_caption(cache, name, selected, width, selected_color);
    DrawResult result{ImGui::GetCursorScreenPos(), ImGui::GetItemRectMin(),
                      ImGui::GetItemRectMax(), {}};
    result.vertices.assign(list->VtxBuffer.Data + first, list->VtxBuffer.Data + list->VtxBuffer.Size);
    frame.end();
    return result;
}

void same_draw(const DrawResult &actual, const DrawResult &expected)
{
    CHECK(actual.cursor.x == expected.cursor.x);
    CHECK(actual.cursor.y == expected.cursor.y);
    CHECK(actual.item_min.x == expected.item_min.x);
    CHECK(actual.item_min.y == expected.item_min.y);
    CHECK(actual.item_max.x == expected.item_max.x);
    CHECK(actual.item_max.y == expected.item_max.y);
    REQUIRE(actual.vertices.size() == expected.vertices.size());
    for (size_t i = 0; i < actual.vertices.size(); ++i) {
        const auto &a = actual.vertices[i];
        const auto &b = expected.vertices[i];
        CHECK(a.pos.x == b.pos.x); CHECK(a.pos.y == b.pos.y);
        CHECK(a.uv.x == b.uv.x); CHECK(a.uv.y == b.uv.y);
        CHECK(a.col == b.col);
    }
}
}

TEST_CASE("asset caption warm cache removes repeated measurements with exact old bytes")
{
    CaptionFrame frame;
    frame.begin();
    std::vector<std::string> names{"", "a", "grass.glb", "file with spaces and dots.001.glb",
        "Detail07_meadow_groundcover_m1_m3_mesh.glb", "literal##filename###suffix.glb",
        "\xe6\xa0\x91\xe5\x8f\xb6_\xe7\x9f\xbf\xe6\xb4\x9e_very_long_name.glb",
        "emoji_\xf0\x9f\x8c\xb3_long_long_long_filename.glb",
        std::string(255, 'W'), std::string(256, 'W'), std::string(350, 'W')};
    size_t cold_calls = 0, warm_calls = 0;
    for (float width : {1.0f, 17.25f, 64.0f, 128.0f, 4096.0f}) {
        const auto key = asset_grid_caption_metrics(width);
        for (const auto &name : names) {
            AssetGridCaptionCache cache;
            CountedHeight measure;
            auto counted = [&](const char *s, float w) { return measure(s, w); };
            const std::string old = original_caption(name, width, key.max_height);
            CHECK(std::string(asset_grid_caption_text(cache, name, key, counted)) == old);
            CHECK(measure.calls > 0);
            cold_calls += measure.calls;
            measure.calls = 0;
            for (unsigned i = 0; i < 12; ++i)
                CHECK(std::string(asset_grid_caption_text(cache, name, key, counted)) == old);
            CHECK(measure.calls == 0);
            warm_calls += measure.calls;
            CHECK(cache.clipped_text.size() <= 255);
            CHECK(cache.source_name == name);
        }
    }
    printf("CAPTION_COUNTS cases=55 repeated_frames=12 cold_calls=%zu warm_calls=%zu cache_object_bytes=%zu\n",
           cold_calls, warm_calls, sizeof(AssetGridCaptionCache));
    frame.end();
}

TEST_CASE("asset caption exact name and every metric identity invalidate derived text")
{
    CaptionFrame frame;
    frame.begin();
    const auto key = asset_grid_caption_metrics(64.0f);
    const std::string name = "a_long_model_name_for_cache_validation.glb";
    AssetGridCaptionCache cache;
    CountedHeight measure;
    auto counted = [&](const char *s, float w) { return measure(s, w); };
    asset_grid_caption_text(cache, name, key, counted);
    std::string renamed = name;
    renamed[0] = 'W';
    measure.calls = 0;
    CHECK(std::string(asset_grid_caption_text(cache, renamed, key, counted)) ==
          original_caption(renamed, key.wrap_width, key.max_height));
    CHECK(measure.calls > 0);

    std::vector<AssetGridCaptionMetrics> changes;
    auto changed = key;
    changed.context = nullptr; changes.push_back(changed);
    changed = key; changed.font = nullptr; changes.push_back(changed);
    changed = key; changed.atlas = nullptr; changes.push_back(changed);
    changed = key; ++changed.font_id; changes.push_back(changed);
    changed = key; ++changed.next_font_id; changes.push_back(changed);
    changed = key; ++changed.next_texture_id; changes.push_back(changed);
    changed = key; changed.size += 1.25f; changes.push_back(changed);
    changed = key; changed.dpi += 0.5f; changes.push_back(changed);
    changed = key; changed.spacing_x += 0.5f; changes.push_back(changed);
    changed = key; changed.spacing_y += 0.25f; changes.push_back(changed);
    changed = key; changed.wrap_width = 96.0f; changes.push_back(changed);
    changed = key; changed.max_height += 14.0f; changes.push_back(changed);
    for (const auto &next : changes) {
        asset_grid_caption_text(cache, name, key, counted);
        measure.calls = 0;
        CHECK(std::string(asset_grid_caption_text(cache, name, next, counted)) ==
              original_caption(name, next.wrap_width, next.max_height));
        CHECK(measure.calls > 0);
        measure.calls = 0;
        asset_grid_caption_text(cache, name, next, counted);
        CHECK(measure.calls == 0);
    }
    frame.end();
}

TEST_CASE("asset caption same owner preserves selected full text and fractional layout")
{
    CaptionFrame frame;
    const std::string name = "Detail07_many_wrapped_lines_remain_full_when_selected.glb";
    AssetGridCaptionCache cache;
    /* First Begin can hide vertices while the new window settles. */
    draw(frame, cache, "settle", false, true, 64.0f);
    for (float spacing : {4.0f, 3.25f}) {
        ImGui::GetStyle().ItemSpacing = ImVec2(7.5f, spacing);
        for (bool selected : {false, true, false}) {
            const auto expected = draw(frame, cache, name, selected, true, 64.0f);
            const auto actual = draw(frame, cache, name, selected, false, 64.0f);
            same_draw(actual, expected);
        }
    }
    AssetGridCaptionCache selected_only;
    draw(frame, selected_only, name, true, false, 64.0f);
    CHECK_FALSE(selected_only.ready);
}

TEST_CASE("asset caption font reload DPI spacing and context come from real ImGui owners")
{
    CaptionFrame frame;
    AssetGridCaptionCache cache;
    const std::string name = "changing_font_changes_the_grid_caption.glb";
    draw(frame, cache, name, false, false, 64.0f);
    const auto old = cache.metrics;
    ImGui::GetMainViewport()->DpiScale = 1.5f;
    ImGui::GetStyle().ItemSpacing.y = 3.75f;
    draw(frame, cache, name, false, false, 64.0f);
    CHECK(cache.metrics.dpi == 1.5f);
    CHECK(cache.metrics.spacing_y == 3.75f);
    CHECK_FALSE(cache.metrics == old);
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig config;
    config.SizePixels = 22.0f;
    io.FontDefault = io.Fonts->AddFontDefault(&config);
    ImGui::GetStyle().FontSizeBase = config.SizePixels;
    unsigned char *pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    draw(frame, cache, name, false, false, 64.0f);
    CHECK(cache.metrics.font_id != old.font_id);
    CHECK(cache.metrics.size == 22.0f);
    same_draw(draw(frame, cache, name, false, false, 64.0f),
              draw(frame, cache, name, false, true, 64.0f));
    const auto original_context = cache.metrics.context;
    ImGuiContext *previous = ImGui::GetCurrentContext();
    /* The shared fixture configures the current context after CreateContext.
     * Start independently so that setup reaches the new context's IO/font. */
    ImGui::SetCurrentContext(nullptr);
    {
        CaptionFrame other;
        draw(other, cache, name, false, false, 64.0f);
        CHECK(cache.metrics.context != original_context);
        CHECK(cache.metrics.context == other.imgui.context);
    }
    ImGui::SetCurrentContext(previous);
}

TEST_CASE("asset caption filtered copy warms its original entry and refresh retires state")
{
    CaptionFrame frame;
    frame.begin();
    s_assets = AssetBrowserState{};
    s_assets.view_mode = ASSET_BROWSER_VIEW_GRID;
    s_assets.renaming_idx = -1;
    std::vector<FileEntry> source(4);
    for (size_t i = 0; i < source.size(); ++i)
        source[i].name = "long_directory_asset_model_for_filter_" + std::to_string(i) + ".glb";
    s_assets.selected_set.insert(1);
    s_assets.renaming_idx = 2;
    auto view = asset_caption_filtered_copy(source);
    CHECK(source[0].grid_caption.ready);
    CHECK_FALSE(source[1].grid_caption.ready);
    CHECK_FALSE(source[2].grid_caption.ready);
    CHECK(source[3].grid_caption.ready);
    CHECK(view[0].grid_caption.ready);
    CountedHeight measure;
    auto counted = [&](const char *s, float w) { return measure(s, w); };
    const auto key = asset_grid_caption_metrics(64.0f);
    asset_grid_caption_text(source[0].grid_caption, source[0].name, key, counted);
    CHECK(measure.calls == 0);
    view.clear();
    view = asset_caption_filtered_copy(source);
    asset_grid_caption_text(view[0].grid_caption, view[0].name, key, counted);
    CHECK(measure.calls == 0);
    source.clear();
    source.resize(1);
    source[0].name = "refreshed_same_slot_new_entry.glb";
    CHECK_FALSE(source[0].grid_caption.ready);
    CHECK(source[0].grid_caption.source_name.empty());
    s_assets.view_mode = ASSET_BROWSER_VIEW_DETAILS;
    view = asset_caption_filtered_copy(source);
    CHECK_FALSE(source[0].grid_caption.ready);
    frame.end();
}
