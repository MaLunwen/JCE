/*
 * jce_panel_world_streaming.cpp — World-streaming authoring panel.
 *
 * Edits the scene-level JceSceneStreamingSettings block (streamer config
 * + explicit chunk registry) that jce_scene_components_json.c serializes
 * as the scene's "streaming" object.  All mutations go through
 * jce_scene_get_streaming_settings_mut() wrapped in begin/end_batch_edit,
 * so undo/redo comes for free via the scene-JSON snapshot history.
 *
 * The "Preview" toggle (session-local, default off) drives the editor's
 * live streamer: jce_editor_scene_render_streaming_rebuild() recreates
 * the JceWorldStreamer from the authored settings — the config struct has
 * no setters, so EVERY settings change recreates the streamer.  Preview
 * spawns/destroys real chunk entities in the hierarchy as the editor
 * camera moves; saving the scene is safe (the streamer is destroyed
 * before serialization — see jce_state_save_scene_file).
 */

#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"
#include "scene/jce_editor_scene_render.h"
#include "ui/jce_editor_panels.h"
#include "dialogs/jce_path_input.h"

#include <jce/tools/jce_imgui.hpp>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_camera.h>
#include <jce/resource/jce_world_streamer.h>
}

#include <cfloat>
#include <cstdio>
#include <cstring>

namespace {

/* Rebuild the preview streamer only once the active edit (drag) ends, so
 * radius scrubbing doesn't recreate the streamer every frame. */
bool g_rebuild_pending = false;

void request_rebuild(void)
{
    g_rebuild_pending = true;
}

void flush_pending_rebuild(void)
{
    if (!g_rebuild_pending) return;
    if (ImGui::IsAnyItemActive()) return;   /* wait for the drag to end */
    g_rebuild_pending = false;
    jce_editor_scene_render_streaming_rebuild();
}

/* Smallest id not used by any authored chunk. */
uint32_t next_free_chunk_id(const JceSceneStreamingSettings *st)
{
    uint32_t id = 0;
    for (;;) {
        bool taken = false;
        for (uint32_t i = 0; i < st->chunk_count; ++i) {
            if (st->chunks[i].id == id) { taken = true; break; }
        }
        if (!taken) return id;
        ++id;
    }
}

const char *chunk_state_label(JceChunkState state)
{
    switch (state) {
    case JCE_CHUNK_LOADED:    return jce_editor_i18n("panel.streaming.state.loaded");
    case JCE_CHUNK_LOADING:   return jce_editor_i18n("panel.streaming.state.loading");
    case JCE_CHUNK_UNLOADING: return jce_editor_i18n("panel.streaming.state.unloading");
    case JCE_CHUNK_UNLOADED:
    default:                  return jce_editor_i18n("panel.streaming.state.unloaded");
    }
}

ImVec4 chunk_state_color(JceChunkState state)
{
    switch (state) {
    case JCE_CHUNK_LOADED:    return ImVec4(0.35f, 0.90f, 0.35f, 1.0f);
    case JCE_CHUNK_LOADING:
    case JCE_CHUNK_UNLOADING: return ImVec4(0.95f, 0.85f, 0.30f, 1.0f);
    case JCE_CHUNK_UNLOADED:
    default:                  return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    }
}

/* Commit a locally edited copy back into the scene inside one undo batch.
 * The undo system snapshots the full scene JSON (which now includes the
 * "streaming" block), so this is all the history wiring needed. */
void commit_settings(JceScene *scene, const JceSceneStreamingSettings &edited)
{
    jce_state_begin_batch_edit();
    JceSceneStreamingSettings *mut = jce_scene_get_streaming_settings_mut(scene);
    if (mut) *mut = edited;
    jce_state_end_batch_edit();
    request_rebuild();
}

void draw_config(JceScene *scene, JceSceneStreamingSettings &st)
{
    bool changed = false;

    changed |= ImGui::Checkbox(
        jce_editor_i18n_id("panel.streaming.enable", "ws_enable"),
        &st.enabled);

    {
        const char *modes[2] = {
            jce_editor_i18n("panel.streaming.mode.radial"),
            jce_editor_i18n("panel.streaming.mode.rectangular"),
        };
        int mode = (st.mode == 1) ? 1 : 0;
        if (ImGui::Combo(jce_editor_i18n_id("panel.streaming.mode", "ws_mode"),
                         &mode, modes, 2)) {
            st.mode = mode;
            changed = true;
        }
    }

    changed |= ImGui::DragFloat(
        jce_editor_i18n_id("panel.streaming.loadRadius", "ws_loadr"),
        &st.load_radius, 1.0f, 1.0f, 100000.0f, "%.0f m");
    changed |= ImGui::DragFloat(
        jce_editor_i18n_id("panel.streaming.unloadRadius", "ws_unloadr"),
        &st.unload_radius, 1.0f, 1.0f, 100000.0f, "%.0f m");
    /* Authoring invariant: unload must not undercut load (the streamer
     * would thrash load/unload at the boundary). */
    if (st.load_radius < 1.0f) st.load_radius = 1.0f;
    if (st.unload_radius < st.load_radius) st.unload_radius = st.load_radius;

    {
        int budget = (int)st.budget_mb;
        if (ImGui::DragInt(
                jce_editor_i18n_id("panel.streaming.budgetMb", "ws_budget"),
                &budget, 1.0f, 0, 16384, "%d MiB")) {
            st.budget_mb = (budget < 0) ? 0u : (uint32_t)budget;
            changed = true;
        }
        int pending = (int)st.max_pending;
        if (ImGui::DragInt(
                jce_editor_i18n_id("panel.streaming.maxPending", "ws_pending"),
                &pending, 0.1f, 1, 64)) {
            st.max_pending = (pending < 1) ? 1u : (uint32_t)pending;
            changed = true;
        }
        changed |= ImGui::DragFloat(
            jce_editor_i18n_id("panel.streaming.frameBudgetMs", "ws_framems"),
            &st.frame_budget_ms, 0.1f, 0.1f, 16.0f, "%.1f ms");
        if (st.frame_budget_ms < 0.1f) st.frame_budget_ms = 0.1f;
    }

    if (changed)
        commit_settings(scene, st);
}

void draw_chunk_table(JceScene *scene, JceSceneStreamingSettings &st,
                      JceWorldStreamer *ws)
{
    bool     changed    = false;
    int      delete_idx = -1;
    const bool previewing = (ws != NULL);

    const int cols = previewing ? 6 : 5;
    if (ImGui::BeginTable("ws_chunks", cols,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn(jce_editor_i18n("panel.streaming.col.id"),
                                ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.streaming.col.center"));
        ImGui::TableSetupColumn(jce_editor_i18n("panel.streaming.col.radius"),
                                ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.streaming.col.path"));
        if (previewing)
            ImGui::TableSetupColumn(
                jce_editor_i18n("panel.streaming.col.state"),
                ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.streaming.col.actions"),
                                ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        for (uint32_t i = 0; i < st.chunk_count; ++i) {
            JceSceneStreamChunk &c = st.chunks[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);

            ImGui::TableNextColumn();
            ImGui::Text("%u", c.id);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::DragFloat3("##ws_center", c.center, 0.5f);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::DragFloat("##ws_radius", &c.radius,
                                        0.5f, 0.0f, 100000.0f, "%.0f");
            if (c.radius < 0.0f) c.radius = 0.0f;

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            {
                /* Project-relative scene-fragment path (same base the
                 * streamer FS mounts — the project root). */
                JcePathInputOpts opts;
                opts.title = jce_editor_i18n("panel.streaming.col.path");
                changed |= jce_draw_path_input("##ws_path", c.path,
                                               sizeof(c.path),
                                               JcePathKind::AssetVfs, &opts);
            }

            if (previewing) {
                ImGui::TableNextColumn();
                JceChunkState cs = jce_world_streamer_chunk_state(ws, c.id);
                ImGui::TextColored(chunk_state_color(cs), "%s",
                                   chunk_state_label(cs));
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton(
                    jce_editor_i18n_id("panel.streaming.useCamera", "ws_cam"))) {
                JceCamera *cam = jce_editor_scene_get_camera();
                if (cam) {
                    jce_vec3 eye = jce_camera_get_position(cam);
                    c.center[0] = eye.x;
                    c.center[1] = eye.y;
                    c.center[2] = eye.z;
                    changed = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(
                    jce_editor_i18n_id("panel.streaming.delete", "ws_del")))
                delete_idx = (int)i;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (delete_idx >= 0 && (uint32_t)delete_idx < st.chunk_count) {
        for (uint32_t i = (uint32_t)delete_idx; i + 1 < st.chunk_count; ++i)
            st.chunks[i] = st.chunks[i + 1];
        st.chunk_count--;
        memset(&st.chunks[st.chunk_count], 0, sizeof(st.chunks[0]));
        changed = true;
    }

    /* Add row — refuse chunk #257 (the streamer's roster pool is fixed at
     * JCE_SCENE_MAX_STREAM_CHUNKS; extra chunks would be silently inert). */
    if (st.chunk_count >= JCE_SCENE_MAX_STREAM_CHUNKS) {
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.streaming.chunkLimit"));
    } else if (ImGui::Button(
                   jce_editor_i18n_id("panel.streaming.addChunk", "ws_add"))) {
        JceSceneStreamChunk &c = st.chunks[st.chunk_count];
        memset(&c, 0, sizeof(c));
        c.id     = next_free_chunk_id(&st);
        c.radius = 50.0f;
        JceCamera *cam = jce_editor_scene_get_camera();
        if (cam) {
            jce_vec3 eye = jce_camera_get_position(cam);
            c.center[0] = eye.x;
            c.center[1] = eye.y;
            c.center[2] = eye.z;
        }
        st.chunk_count++;
        changed = true;
    }

    if (changed)
        commit_settings(scene, st);
}

void draw_content(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) {
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.streaming.noScene"));
        return;
    }

    flush_pending_rebuild();

    /* Edit a local copy; commit_settings() writes it back through the
     * _mut accessor inside one undo batch when something changed.  The
     * block is ~70 KB — static keeps it off the ImGui draw stack. */
    static JceSceneStreamingSettings s_edit;
    const JceSceneStreamingSettings *cur = jce_scene_get_streaming_settings(scene);
    s_edit = cur ? *cur : jce_scene_streaming_settings_default();

    /* Session preview toggle — NOT part of the scene data / undo history. */
    {
        bool preview = jce_state_get_streaming_preview();
        if (ImGui::Checkbox(
                jce_editor_i18n_id("panel.streaming.preview", "ws_preview"),
                &preview)) {
            jce_state_set_streaming_preview(preview);
            jce_editor_scene_render_streaming_rebuild();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("panel.streaming.help"));
    }
    ImGui::Separator();

    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.streaming.section.config"),
                                ImGuiTreeNodeFlags_DefaultOpen))
        draw_config(scene, s_edit);

    ImGui::Separator();

    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.streaming.section.chunks"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        JceWorldStreamer *ws = jce_state_get_streaming_preview()
                             ? jce_editor_get_world_streamer() : NULL;
        draw_chunk_table(scene, s_edit, ws);
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", jce_editor_i18n("panel.streaming.help"));
}

} /* namespace */

extern "C" void jce_editor_panel_world_streaming(void)
{
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###world_streaming",
             jce_editor_i18n("window.worldStreaming"));
    if (!ImGui::Begin(_wt,
                      jce_editor_panel_visible_ptr(JCE_PANEL_WORLD_STREAMING),
                      ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }
    draw_content();
    ImGui::End();
}
