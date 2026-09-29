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
#include "core/jce_project_settings.h"
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
    /* What this panel actually writes now.  It said
     * "Settings/TagsAndLayers.json" -- the file only this panel ever read --
     * so the status line named a path whose contents changed nothing. */
    return ".jce/project-settings.json";
}

/* PROJECT SETTINGS IS THE AUTHORITY, and this panel used to edit something
 * else entirely.
 *
 * There were two 32-slot layer-name registries and nothing copied between
 * them.  This panel wrote jce_layer_set_name() -> Settings/TagsAndLayers.json,
 * whose ONLY reader in the whole tree was this panel.  Every consumer --
 * the entity Layer combo, the physics layer combo, the camera culling-mask
 * dropdown, and the build's push into the shipped physics layer names --
 * reads JceProjectSettings::tags_layers.  So renaming layer 8 here changed
 * nothing anywhere, and Project Settings > Tags and Layers, which edits the
 * store that IS read, showed a different set of names in a panel with the
 * same title.  Same split for tags: this panel called jce_tag_intern() while
 * the Inspector's tag combo reads ps->tags_layers.tags[].
 *
 * jce_scene.h also said the engine registry was "loaded on scene init via
 * jce_scene_tags_layers_load()".  The only caller was the line below.
 *
 * The engine registry is still MIRRORED, because jce_layer_name() is public
 * API a user project may call and it must not disagree with the editor. */
void sync_engine_registry_from(const JceProjectSettings *ps)
{
    if (!ps) return;
    for (uint32_t i = 0; i < JCE_LAYER_COUNT; i++)
        jce_layer_set_name((uint8_t)i, ps->tags_layers.layers[i]);
}

void sync_layer_buf_from_settings(void)
{
    const JceProjectSettings *ps = jce_project_settings_current();
    for (uint32_t i = 0; i < JCE_LAYER_COUNT; i++) {
        const char *nm = ps ? ps->tags_layers.layers[i] : "";
        std::snprintf(g_st.layer_buf[i], sizeof(g_st.layer_buf[i]),
                      "%s", nm ? nm : "");
    }
}

void load_if_needed(void)
{
    if (g_st.loaded) return;
    g_st.loaded = true;
    /* No mirroring here: jce_project_settings_apply() does it for every
     * consumer, whether or not this panel was ever opened. */
    JceProjectSettings ps;
    if (jce_project_settings_load(&ps))
        jce_project_settings_apply(&ps);
    sync_layer_buf_from_settings();
}

/* Write the panel's buffers into project settings and persist THAT.  The
 * engine registry is mirrored so jce_layer_name() agrees. */
bool commit_layers(void)
{
    JceProjectSettings ps;
    if (!jce_project_settings_load(&ps))
        jce_project_settings_defaults(&ps);
    for (uint32_t i = 0; i < JCE_LAYER_COUNT; i++)
        std::snprintf(ps.tags_layers.layers[i], JCE_PS_NAME_LEN,
                      "%s", g_st.layer_buf[i]);
    /* Tags travel the same way: the engine tag pool is where this panel does
     * its add/remove, and the Inspector's tag combo reads
     * ps->tags_layers.tags[].  Copy the pool across so both agree. */
    int n = jce_tag_count();
    if (n > JCE_PS_MAX_TAGS) n = JCE_PS_MAX_TAGS;
    ps.tags_layers.tag_count = n;
    for (int i = 0; i < n; ++i) {
        const char *nm = jce_tag_at(i);
        std::snprintf(ps.tags_layers.tags[i], JCE_PS_NAME_LEN,
                      "%s", nm ? nm : "");
    }
    if (!jce_project_settings_save(&ps)) return false;
    jce_project_settings_apply(&ps);
    sync_engine_registry_from(&ps);
    return true;
}

void save_now(void)
{
    if (!commit_layers()) {
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
                    ImGui::InputText("##layer_name", g_st.layer_buf[i],
                                     sizeof(g_st.layer_buf[i]));
                    /* Commit on deactivate only: project settings is a
                     * whole-file save, and writing it per keystroke would
                     * rewrite the document 30 times a second. */
                    if (ImGui::IsItemDeactivatedAfterEdit()) save_now();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::Button(jce_editor_i18n("panel.tags_layers.reset_default"))) {
            jce_layer_reset_defaults();
            /* Defaults live in the engine registry; copy them ACROSS to the
             * authoritative store rather than leaving the two disagreeing. */
            for (uint32_t i = 0; i < JCE_LAYER_COUNT; i++) {
                const char *nm = jce_layer_name(i);
                std::snprintf(g_st.layer_buf[i], sizeof(g_st.layer_buf[i]),
                              "%s", nm ? nm : "");
            }
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
