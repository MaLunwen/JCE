/*
 * jce_panel_tags_layers.cpp  Tags & Layers panel (P4-A.4).
 *
 * Unity parity: `Edit > Project Settings > Tags and Layers`.  Edits the
 * engine-level scene tag registry (interned strings, up to 1024 slots)
 * and 32 named layer slots.  Persists to
 * `<project>/Settings/TagsAndLayers.json` on every change.  Slot 0 of
 * the tag table is reserved as "Untagged" and cannot be removed.
 * Layers 0..4 are pre-populated with Unity defaults
 * (Default / TransparentFX / Ignore Raycast / Water / UI).
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/api_scene.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
}

#define LOG_TAG "panel_tags_layers"

namespace {

struct State {
    bool loaded      = false;
    char status[256] = {0};
    char new_tag[JCE_TAG_NAME_MAX] = {0};
    char layer_buf[JCE_LAYER_COUNT][JCE_LAYER_NAME_MAX] = {{0}};
};

State g_st;

const char *settings_path(void)
{
    static char path[512];
    /* Persisted under cwd-relative Settings/ — matches Unity convention. */
    std::snprintf(path, sizeof(path), "Settings/TagsAndLayers.json");
    return path;
}

void sync_layer_buf_from_engine(void)
{
    for (uint32_t i = 0; i < JCE_LAYER_COUNT; i++) {
        const char *nm = jce_layer_name(i);
        std::snprintf(g_st.layer_buf[i], sizeof(g_st.layer_buf[i]),
                      "%s", nm ? nm : "");
    }
}

void load_if_needed(void)
{
    if (g_st.loaded) return;
    g_st.loaded = true;
    (void)jce_scene_tags_layers_load(NULL);
    sync_layer_buf_from_engine();
}

void save_now(void)
{
    if (!jce_scene_tags_layers_save(NULL)) {
        std::snprintf(g_st.status, sizeof(g_st.status), "%s",
                      jce_editor_i18n("panel.tags_layers.save_failed"));
    } else {
        std::snprintf(g_st.status, sizeof(g_st.status), "%s",
                      settings_path());
    }
}

} /* namespace */

extern "C" void jce_editor_panel_tags_layers_content(void)
{
    load_if_needed();

    ImGui::TextDisabled("%s: %s",
                        jce_editor_i18n("panel.tags_layers.title"),
                        g_st.status[0] ? g_st.status : settings_path());
    ImGui::Separator();

    /* ---- Tags section ---- */
    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.tags_layers.tags"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##tag_list", 2,
                ImGuiTableFlags_BordersInner | ImGuiTableFlags_SizingFixedFit,
                ImVec2(0, 0))) {
            ImGui::TableSetupColumn(jce_editor_i18n("panel.tags_layers.tags"),
                ImGuiTableColumnFlags_WidthStretch, 260.f);
            ImGui::TableSetupColumn("",
                ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            uint32_t count = (uint32_t)jce_tag_count();
            for (uint32_t i = 0; i < count; i++) {
                const char *nm = jce_tag_at((int)i);
                if (!nm) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (i == 0) {
                    ImGui::TextDisabled("%s", nm);
                } else {
                    ImGui::TextUnformatted(nm);
                }
                ImGui::TableSetColumnIndex(1);
                if (i == 0) {
                    ImGui::TextDisabled("--");
                } else {
                    ImGui::PushID((int)i);
                    if (ImGui::SmallButton("X")) {
                        if (jce_tag_remove(nm))
                            save_now();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::SetNextItemWidth(220.f);
        ImGui::InputText("##new_tag", g_st.new_tag, sizeof(g_st.new_tag));
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("panel.tags_layers.add_tag"))) {
            if (g_st.new_tag[0]) {
                if (jce_tag_intern(g_st.new_tag) != 0) {
                    g_st.new_tag[0] = 0;
                    save_now();
                }
            }
        }
    }

    ImGui::Spacing();

    /* ---- Layers section ---- */
    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.tags_layers.layers"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##layer_list", 2,
                ImGuiTableFlags_BordersInner | ImGuiTableFlags_SizingFixedFit,
                ImVec2(0, 0))) {
            ImGui::TableSetupColumn("#",
                ImGuiTableColumnFlags_WidthFixed, 36);
            ImGui::TableSetupColumn(jce_editor_i18n("panel.tags_layers.layers"),
                ImGuiTableColumnFlags_WidthStretch, 260.f);
            ImGui::TableHeadersRow();

            for (uint32_t i = 0; i < JCE_LAYER_COUNT; i++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u", i);
                ImGui::TableSetColumnIndex(1);
                ImGui::PushID((int)i);
                ImGui::SetNextItemWidth(-FLT_MIN);
                /* Unity convention: slots 0..4 are builtin defaults
                   ("Default", "TransparentFX", "Ignore Raycast",
                   "Water", "UI") — render them read-only. */
                if (i <= 4) {
                    ImGui::TextDisabled("%s", g_st.layer_buf[i]);
                } else {
                    if (ImGui::InputText("##layer_name", g_st.layer_buf[i],
                                         sizeof(g_st.layer_buf[i]))) {
                        jce_layer_set_name((uint8_t)i, g_st.layer_buf[i]);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) save_now();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::Button(jce_editor_i18n("panel.tags_layers.reset_default"))) {
            jce_layer_reset_defaults();
            sync_layer_buf_from_engine();
            save_now();
        }
    }
}

extern "C" void jce_editor_panel_tags_layers(void)
{
    bool *visible = jce_editor_panel_visible_ptr(JCE_PANEL_TAGS_LAYERS);
    if (!visible || !*visible) return;
    char title[64];
    std::snprintf(title, sizeof(title), "%s###tags_layers",
                  jce_editor_i18n("panel.tags_layers.title"));
    if (ImGui::Begin(title, visible, ImGuiWindowFlags_NoFocusOnAppearing))
        jce_editor_panel_tags_layers_content();
    ImGui::End();
}
