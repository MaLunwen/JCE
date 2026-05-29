/*
 * jce_path_input.cpp  Implementation of the unified path-input widget.
 *
 * Per-label state (browse_ready / cancel flags + last_seen buffer for
 * change detection) is keyed by ImGui::GetID(label) so each call site
 * gets independent state without callers having to declare statics.
 */

#include "jce_path_input.h"
#include "jce_editor_dialogs_internal.h"
#include "core/jce_editor_i18n.h"
#include "jce_dialog_asset_picker.h"
#include "ui/jce_editor_dnd.h"
#include "ui/jce_editor_panels.h"  /* jce_editor_path_to_relative */

#include <imgui.h>
#include "core/jce_assetdb.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace {

struct Slot {
    bool   browse_ready    = false;
    bool   browse_cancel   = false;
    char   last_seen[1024] = {0};
};

std::unordered_map<ImGuiID, Slot> g_slots;

Slot &slot_for(const char *label)
{
    ImGuiID id = ImGui::GetID(label);
    return g_slots[id];
}

/* Browse glyph.  Single-byte "..." keeps font requirements minimal —
 * the bundled OFL font and most system CJK fonts render it cleanly.  */
const char *kBrowseGlyph = "...";
const char *kClearGlyph  = "x";

/* Per-kind placeholder hint shown when the buffer is empty. */
const char *placeholder_for(JcePathKind kind, int asset_kind)
{
    switch (kind) {
    case JcePathKind::FileAbs:
        return jce_editor_i18n_or("pathInput.hint.file",
                                  "(empty)  click ... to browse a file");
    case JcePathKind::FolderAbs:
        return jce_editor_i18n_or("pathInput.hint.folder",
                                  "(empty)  click ... to pick a folder");
    case JcePathKind::SaveFileAbs:
        return jce_editor_i18n_or("pathInput.hint.save",
                                  "(empty)  click ... to choose a save target");
    case JcePathKind::AssetVfs:
        if (asset_kind == JCE_ASSET_KIND_TEXTURE)
            return jce_editor_i18n_or("pathInput.hint.texture",
                                      "(empty)  pick a texture from project assets");
        if (asset_kind == JCE_ASSET_KIND_MODEL)
            return jce_editor_i18n_or("pathInput.hint.model",
                                      "(empty)  pick a model from project assets");
        if (asset_kind == JCE_ASSET_KIND_AUDIO)
            return jce_editor_i18n_or("pathInput.hint.audio",
                                      "(empty)  pick an audio clip");
        if (asset_kind == JCE_ASSET_KIND_MATERIAL)
            return jce_editor_i18n_or("pathInput.hint.material",
                                      "(empty)  pick a material");
        if (asset_kind == JCE_ASSET_KIND_SCRIPT)
            return jce_editor_i18n_or("pathInput.hint.script",
                                      "(empty)  pick a script");
        return jce_editor_i18n_or("pathInput.hint.asset",
                                  "(empty)  pick a project asset");
    }
    return "";
}

/* Browse-button tooltip — kind-aware so users understand what they get. */
const char *tooltip_for(JcePathKind kind)
{
    switch (kind) {
    case JcePathKind::FileAbs:     return jce_editor_i18n_or("pathInput.tip.file",   "Browse for a file...");
    case JcePathKind::FolderAbs:   return jce_editor_i18n_or("pathInput.tip.folder", "Choose a folder...");
    case JcePathKind::SaveFileAbs: return jce_editor_i18n_or("pathInput.tip.save",   "Choose where to save...");
    case JcePathKind::AssetVfs:    return jce_editor_i18n_or("pathInput.tip.asset",  "Pick from project assets...");
    }
    return "Browse...";
}

} /* namespace */

bool jce_draw_path_input(const char *label,
                         char *buf, size_t buf_size,
                         JcePathKind kind,
                         const JcePathInputOpts *opts_in)
{
    if (!buf || buf_size == 0 || !label) return false;
    JcePathInputOpts defaults;
    const JcePathInputOpts *opts = opts_in ? opts_in : &defaults;

    Slot &slot = slot_for(label);

    /* Stable unique ids for the buttons.  We append the label's ImGuiID
     * hash so multiple path inputs on the same panel don't collide
     * (ImGui's "##" rule uses only the suffix after the last ##). */
    ImGuiID lblHash = ImGui::GetID(label);
    char btn_id[64];
    snprintf(btn_id, sizeof(btn_id), "...##__br_%08X", (unsigned)lblHash);
    char clr_id[64];
    snprintf(clr_id, sizeof(clr_id), "x##__cl_%08X",  (unsigned)lblHash);

    const ImGuiStyle &st = ImGui::GetStyle();
    const float button_w = ImGui::GetFrameHeight();      /* square */
    const float spacing  = st.ItemInnerSpacing.x;
    const float avail    = ImGui::CalcItemWidth();
    /* Reserve room for browse + (optional) clear button. */
    const bool  show_clear = (buf[0] != '\0');
    const float reserved = button_w + spacing + (show_clear ? (button_w + spacing) : 0.0f);
    const float input_w  = (opts->width > 0)
        ? opts->width
        : ((avail > reserved) ? (avail - reserved) : avail);

    bool changed = false;

    auto draw_browse_button = [&]() {
        if (ImGui::Button(btn_id, ImVec2(button_w, 0))) {
            const char *title = (opts->title && opts->title[0]) ? opts->title : label;
            switch (kind) {
            case JcePathKind::FileAbs:
                open_file_dialog_async(title, buf, opts->filter,
                                       buf, buf_size,
                                       &slot.browse_ready, &slot.browse_cancel);
                break;
            case JcePathKind::FolderAbs:
                pick_folder_dialog_async(title, buf,
                                         buf, buf_size, nullptr, 0,
                                         &slot.browse_ready, &slot.browse_cancel);
                break;
            case JcePathKind::SaveFileAbs:
                save_file_dialog_async(title, buf, opts->filter,
                                       buf, buf_size,
                                       &slot.browse_ready, &slot.browse_cancel);
                break;
            case JcePathKind::AssetVfs:
                jce_editor_asset_picker_open(title, opts->asset_kind,
                                             buf, buf_size,
                                             &slot.browse_ready,
                                             &slot.browse_cancel);
                break;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip_for(kind));
    };

    auto draw_clear_button = [&]() {
        /* Subtler styling: dim text so it doesn't compete with browse. */
        ImGui::PushStyleColor(ImGuiCol_Text, st.Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Button(clr_id, ImVec2(button_w, 0))) {
            buf[0] = '\0';
            slot.last_seen[0] = '\0';
            changed = true;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                jce_editor_i18n_or("pathInput.clear.tooltip", "Clear"));
    };

    if (opts->button_first) {
        draw_browse_button();
        ImGui::SameLine(0.0f, spacing);
    }

    ImGui::PushItemWidth(input_w);
    const char *hint = placeholder_for(kind, opts->asset_kind);
    if (ImGui::InputTextWithHint(label, hint, buf, buf_size)) changed = true;
    /* Hover tooltip: full path (useful when truncated in narrow panels). */
    if (buf[0] != '\0' && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", buf);
    /* Drag-drop target: accept asset paths from the asset browser (and any
     * other source that publishes JCE_DND_ASSET_PATH).  For AssetVfs kind
     * we relativize against the project root so the saved value stays
     * portable; for *Abs kinds we keep the dropped path verbatim. */
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *pl =
                ImGui::AcceptDragDropPayload(JCE_DND_ASSET_PATH)) {
            const char *src = (const char *)pl->Data;
            if (src && src[0]) {
                if (kind == JcePathKind::AssetVfs) {
                    char rel[1024];
                    jce_editor_path_to_relative(rel, sizeof(rel), src);
                    snprintf(buf, buf_size, "%s", rel[0] ? rel : src);
                } else {
                    snprintf(buf, buf_size, "%s", src);
                }
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopItemWidth();

    if (!opts->button_first) {
        ImGui::SameLine(0.0f, spacing);
        draw_browse_button();
        if (show_clear) {
            ImGui::SameLine(0.0f, spacing);
            draw_clear_button();
        }
    }

    /* Detect external buffer changes (browse-async result writes here). */
    if (strncmp(slot.last_seen, buf, sizeof(slot.last_seen)) != 0) {
        snprintf(slot.last_seen, sizeof(slot.last_seen), "%s", buf);
        if (slot.browse_ready) {
            slot.browse_ready = false;
            changed = true;
        }
    }
    if (slot.browse_cancel) slot.browse_cancel = false;

    /* Suppress unused glyph warning when only browse is shown. */
    (void)kBrowseGlyph; (void)kClearGlyph;
    return changed;
}
