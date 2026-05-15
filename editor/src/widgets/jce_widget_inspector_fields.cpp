/*
 * jce_widget_inspector_fields.cpp  Standalone ImGui drawers for
 * Vector2Int / Vector3Int / Rect / Bounds / LayerMask / Tag picker
 * + a reset-to-default context-menu helper.
 *
 * Mirrors Unity Inspector field types one-for-one.  Each widget is a
 * thin ImGui binding — no state of its own; the caller owns storage.
 */

#include "jce_widget_inspector_fields.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>

extern "C" bool jce_widget_vec2int(const char *label, int32_t v[2])
{
    return ImGui::InputInt2(label, v);
}

extern "C" bool jce_widget_vec3int(const char *label, int32_t v[3])
{
    return ImGui::InputInt3(label, v);
}

extern "C" bool jce_widget_rect(const char *label, float r[4])
{
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::Indent();
    changed |= ImGui::DragFloat2("Pos##rect",  r,     0.5f);
    changed |= ImGui::DragFloat2("Size##rect", r + 2, 0.5f);
    ImGui::Unindent();
    ImGui::PopID();
    return changed;
}

extern "C" bool jce_widget_bounds(const char *label,
                                   float center[3], float extents[3])
{
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::Indent();
    changed |= ImGui::DragFloat3("Center",   center,  0.05f);
    changed |= ImGui::DragFloat3("Extents",  extents, 0.05f);
    ImGui::Unindent();
    ImGui::PopID();
    return changed;
}

extern "C" bool jce_widget_layer_mask(const char *label, uint32_t *mask,
                                       const char *const *names)
{
    if (!mask) return false;
    bool changed = false;
    /* Build a preview string from the first 3 set bits. */
    char preview[64] = "Nothing";
    if (*mask == 0xFFFFFFFFu) std::snprintf(preview, sizeof(preview), "Everything");
    else if (*mask != 0) {
        int shown = 0;
        size_t off = 0;
        for (int i = 0; i < 32; ++i) {
            if (!(*mask & (1u << i))) continue;
            const char *n = (names && names[i] && names[i][0])
                            ? names[i] : "Layer";
            int written = std::snprintf(preview + off, sizeof(preview) - off,
                                         "%s%s",
                                         shown ? ", " : "", n);
            if (written < 0) break;
            off += (size_t)written;
            if (++shown >= 3) {
                std::snprintf(preview + off, sizeof(preview) - off, " …");
                break;
            }
        }
    }
    if (ImGui::BeginCombo(label, preview)) {
        bool all = (*mask == 0xFFFFFFFFu);
        if (ImGui::Selectable("Everything", all)) {
            *mask = 0xFFFFFFFFu;
            changed = true;
        }
        if (ImGui::Selectable("Nothing", *mask == 0)) {
            *mask = 0;
            changed = true;
        }
        ImGui::Separator();
        for (int i = 0; i < 32; ++i) {
            const char *n = (names && names[i] && names[i][0])
                            ? names[i] : nullptr;
            if (!n) continue;
            bool on = (*mask & (1u << i)) != 0;
            if (ImGui::Checkbox(n, &on)) {
                if (on) *mask |= (1u << i);
                else    *mask &= ~(1u << i);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

extern "C" bool jce_widget_tag_picker(const char *label, char *value,
                                       size_t cap,
                                       const char *const *tag_names)
{
    if (!value || cap == 0) return false;
    bool changed = false;
    if (ImGui::BeginCombo(label, value[0] ? value : "Untagged")) {
        if (ImGui::Selectable("Untagged", value[0] == 0)) {
            value[0] = '\0';
            changed = true;
        }
        ImGui::Separator();
        if (tag_names) {
            for (int i = 0; tag_names[i]; ++i) {
                bool sel = (std::strncmp(value, tag_names[i], cap) == 0);
                if (ImGui::Selectable(tag_names[i], sel)) {
                    std::strncpy(value, tag_names[i], cap - 1);
                    value[cap - 1] = '\0';
                    changed = true;
                }
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

extern "C" bool jce_widget_field_reset_context(const char *id,
                                                JceFieldResetFn revert_fn,
                                                void *user_data)
{
    bool reset = false;
    if (ImGui::BeginPopupContextItem(id)) {
        if (ImGui::MenuItem("Reset to default")) {
            if (revert_fn) revert_fn(user_data);
            reset = true;
        }
        ImGui::EndPopup();
    }
    return reset;
}
