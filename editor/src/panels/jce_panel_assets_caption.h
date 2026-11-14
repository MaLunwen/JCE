/* Private derived grid captions. FileEntry owns and retires each cache. */
#ifndef JCE_PANEL_ASSETS_CAPTION_H
#define JCE_PANEL_ASSETS_CAPTION_H

#include <jce/tools/jce_imgui.hpp>
#include <stdio.h>
#include <string.h>
#include <string>

struct AssetGridCaptionMetrics {
    ImGuiContext *context = nullptr;
    ImFont *font = nullptr;
    ImFontAtlas *atlas = nullptr;
    ImGuiID font_id = 0;
    int next_font_id = 0, next_texture_id = 0;
    float size = 0.0f, dpi = 0.0f;
    float spacing_x = 0.0f, spacing_y = 0.0f;
    float wrap_width = 0.0f, max_height = 0.0f;
    bool operator==(const AssetGridCaptionMetrics &) const = default;
};

struct AssetGridCaptionCache {
    AssetGridCaptionMetrics metrics;
    std::string source_name;
    std::string clipped_text;
    bool ready = false;
    bool clipped = false;
};

inline AssetGridCaptionMetrics asset_grid_caption_metrics(float wrap_width)
{
    ImFont *font = ImGui::GetFont();
    ImFontAtlas *atlas = font->OwnerAtlas;
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    return {ImGui::GetCurrentContext(), font, atlas, font->FontId,
        atlas->FontNextUniqueID, atlas->TexNextUniqueID, ImGui::GetFontSize(),
        ImGui::GetWindowViewport()->DpiScale, spacing.x, spacing.y,
        wrap_width, ImGui::GetTextLineHeightWithSpacing() * 2.0f};
}

/* Keep the existing byte-prefix/256-byte snprintf behavior, including UTF-8
 * boundaries. This cache changes repetition, not the label's text policy.
 * Measurement is the current font's wrapped height; no path or global LRU is
 * retained. Storage is one source name and at most 255 caption bytes per live
 * entry, and is released with directory/search entry replacement. */
template <typename MeasureHeight>
inline const char *asset_grid_caption_text(AssetGridCaptionCache &cache,
    const std::string &name, const AssetGridCaptionMetrics &metrics,
    MeasureHeight measure_height)
{
    if (cache.ready && cache.metrics == metrics && cache.source_name == name)
        return cache.clipped ? cache.clipped_text.c_str() : name.c_str();

    cache.ready = false;
    cache.source_name = name;
    cache.metrics = metrics;
    cache.clipped = measure_height(name.c_str(), metrics.wrap_width) > metrics.max_height;
    cache.clipped_text.clear();
    if (cache.clipped) {
        const char *src = name.c_str();
        int len = (int)strlen(src);
        char trunc[256];
        int cut = len;
        for (cut = len - 1; cut > 0; cut--) {
            snprintf(trunc, sizeof(trunc), "%.*s...", cut, src);
            if (measure_height(trunc, metrics.wrap_width) <= metrics.max_height)
                break;
        }
        if (cut <= 0)
            snprintf(trunc, sizeof(trunc), "...");
        cache.clipped_text = trunc;
    }
    cache.ready = true;
    return cache.clipped ? cache.clipped_text.c_str() : name.c_str();
}

inline const char *asset_grid_caption_text(AssetGridCaptionCache &cache,
    const std::string &name, const AssetGridCaptionMetrics &metrics)
{
    return asset_grid_caption_text(cache, name, metrics,
        [](const char *text, float width) {
            return ImGui::CalcTextSize(text, nullptr, false, width).y;
        });
}

inline void draw_asset_grid_caption(AssetGridCaptionCache &cache,
    const std::string &name, bool selected, float width, ImVec4 selected_color)
{
    const AssetGridCaptionMetrics metrics = asset_grid_caption_metrics(width);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec4 color = selected ? selected_color : ImGui::GetStyleColorVec4(ImGuiCol_Text);

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    /* Selected names retain their original full text and variable height. */
    ImGui::TextWrapped("%s", selected ? name.c_str() :
        asset_grid_caption_text(cache, name, metrics));
    ImGui::PopStyleColor();
    if (!selected) {
        float used_height = ImGui::GetCursorScreenPos().y - start.y;
        if (used_height < metrics.max_height)
            ImGui::Dummy(ImVec2(0, metrics.max_height - used_height));
    }
    ImGui::PopTextWrapPos();
}

#endif /* JCE_PANEL_ASSETS_CAPTION_H */
