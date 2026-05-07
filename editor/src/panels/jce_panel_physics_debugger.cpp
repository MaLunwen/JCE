/*
 * jce_panel_physics_debugger.cpp  Physics Debugger panel.
 *
 * Visualizes the layer collision matrix from project settings (Physics +
 * Physics 2D), with read-only contact statistics placeholder.
 *
 * Editing the matrix here mirrors the matrix editor in the Physics page
 * of Project Settings — both write the same bit grid and persist via
 * jce_project_settings_save().
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_project_settings.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
}

static void draw_matrix(const char *id, uint32_t matrix[JCE_PS_LAYER_COUNT],
                        const char names[JCE_PS_LAYER_COUNT][JCE_PS_NAME_LEN])
{
    /* Determine highest used layer to keep the grid compact. */
    int last = 0;
    for (int i = 0; i < JCE_PS_LAYER_COUNT; i++)
        if (names[i][0] != '\0') last = i;
    int n = last + 1;
    if (n < 8) n = 8;

    if (ImGui::BeginTable(id, n + 1,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("");
        for (int j = 0; j < n; j++) ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        for (int i = n - 1; i >= 0; i--) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(names[i][0] ? names[i] : "(none)");
            for (int j = 0; j < n; j++) {
                if (j > i) { ImGui::TableSetColumnIndex(j + 1); ImGui::Dummy(ImVec2(18,18)); continue; }
                ImGui::TableSetColumnIndex(j + 1);
                bool on = (matrix[i] >> j) & 1u;
                char cb_id[32]; snprintf(cb_id, sizeof(cb_id), "##%s_%d_%d", id, i, j);
                if (ImGui::Checkbox(cb_id, &on)) {
                    if (on) { matrix[i] |= (1u << j); matrix[j] |= (1u << i); }
                    else    { matrix[i] &= ~(1u << j); matrix[j] &= ~(1u << i); }
                }
            }
        }
        ImGui::EndTable();
    }
}

extern "C" void jce_editor_panel_physics_debugger_content(void)
{
    JceProjectSettings *ps = (JceProjectSettings *)jce_project_settings_current();
    if (!ps) {
        ImGui::TextDisabled("%s", jce_editor_i18n("physicsDebugger.noProjectSettings"));
        return;
    }

    ImGui::TextWrapped("%s", jce_editor_i18n("physicsDebugger.toggleHint"));
    ImGui::Spacing();

    bool dirty = false;
    if (ImGui::CollapsingHeader(jce_editor_i18n("physicsDebugger.matrix3D"), ImGuiTreeNodeFlags_DefaultOpen)) {
        uint32_t before[JCE_PS_LAYER_COUNT];
        memcpy(before, ps->physics.layer_collision_matrix, sizeof(before));
        draw_matrix("phx3d", ps->physics.layer_collision_matrix, ps->tags_layers.layers);
        if (memcmp(before, ps->physics.layer_collision_matrix, sizeof(before)) != 0)
            dirty = true;
    }

    if (ImGui::CollapsingHeader(jce_editor_i18n("physicsDebugger.matrix2D"))) {
        uint32_t before[JCE_PS_LAYER_COUNT];
        memcpy(before, ps->physics2d.layer_collision_matrix, sizeof(before));
        draw_matrix("phx2d", ps->physics2d.layer_collision_matrix, ps->tags_layers.layers);
        if (memcmp(before, ps->physics2d.layer_collision_matrix, sizeof(before)) != 0)
            dirty = true;
    }

    if (ImGui::CollapsingHeader(jce_editor_i18n("physicsDebugger.runtimeStats"))) {
        ImGui::TextDisabled("%s", jce_editor_i18n("physicsDebugger.activeContacts"));
        ImGui::TextDisabled("%s", jce_editor_i18n("physicsDebugger.sleepingBodies"));
        ImGui::TextDisabled("%s", jce_editor_i18n("physicsDebugger.queriesThisFrame"));
    }

    if (dirty) jce_project_settings_save(ps);
}
