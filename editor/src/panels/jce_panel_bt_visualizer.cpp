/*
 * jce_panel_bt_visualizer.cpp -- read-only Behavior Tree visualizer.
 *
 * Shows the structure + status of the BehaviorTree component on the
 * focused entity as an indented pre-order list with status dots:
 *
 *   - Play mode: polls the play runtime's shared JceBtContext through
 *     jce_runtime_bt_tree / jce_bt_node_info every frame.  A
 *     BT::TreeObserver is attached (idempotently) so nodes that reset
 *     to IDLE between ticks still show their last SUCCESS/FAILURE as a
 *     ghost ring + transition counter.  Statuses advance at each
 *     agent's authored tick cadence (tick_hz), not at frame rate.
 *   - Edit mode: a panel-owned JceBtContext with lenient loading
 *     (unknown game actions become inert stubs) parses the authored
 *     XML for structure-only preview.  The context is destroyed +
 *     recreated whenever (entity, tree_path, file mtime) changes —
 *     trees are never individually unloadable, so recycling the whole
 *     context prevents unbounded growth.  Load failures are cached so
 *     a broken file never spams the log per frame.
 *
 * Threading note: the editor steps the play runtime synchronously on
 * the UI thread, so per-frame polling here is safe.  If Play stepping
 * ever moves off-thread, this poll must move behind the step.
 */

#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"
#include "ui/jce_editor_panels.h"
#include "scene/jce_editor_scene_render.h"   /* jce_editor_resolve_asset_path */

#include <jce/tools/jce_imgui.hpp>
extern "C" {
#include <jce/application/jce_runtime.h>
#include <jce/middleware/ai/jce_bt.h>
#include <jce/os/core/jce_filesystem.h>
}

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct State {
    /* Edit-mode standalone context (lenient load, structure only).
     * (JCE_BT_TREE_INVALID is a C compound literal — spell the invalid
     * handle out for C++.) */
    JceBtContext   *ctx  = nullptr;
    JceBtTreeHandle tree { UINT32_MAX };
    bool            attempted   = false;   /* a load attempt is cached */
    bool            load_failed = false;
    uint32_t        loaded_entity = 0;
    char            loaded_path[260] = { 0 };
    int64_t         loaded_mtime = 0;
};

State s;

static ImU32 status_color(JceBtNodeStatus st)
{
    switch (st) {
    case JCE_BT_NODE_RUNNING: return IM_COL32(235, 180,  50, 255);
    case JCE_BT_NODE_SUCCESS: return IM_COL32( 80, 200, 110, 255);
    case JCE_BT_NODE_FAILURE: return IM_COL32(230,  80,  80, 255);
    case JCE_BT_NODE_SKIPPED: return IM_COL32(110, 130, 160, 255);
    case JCE_BT_NODE_IDLE:
    default:                  return IM_COL32(128, 128, 128, 255);
    }
}

static const char *status_label(JceBtNodeStatus st)
{
    switch (st) {
    case JCE_BT_NODE_RUNNING: return jce_editor_i18n("btViz.status.running");
    case JCE_BT_NODE_SUCCESS: return jce_editor_i18n("btViz.status.success");
    case JCE_BT_NODE_FAILURE: return jce_editor_i18n("btViz.status.failure");
    case JCE_BT_NODE_SKIPPED: return jce_editor_i18n("btViz.status.skipped");
    case JCE_BT_NODE_IDLE:
    default:                  return jce_editor_i18n("btViz.status.idle");
    }
}

/* Resolve the authored (usually project-relative) tree path to a host
 * path the BT loader can open.  Falls back to the raw path. */
static const char *resolve_tree_path(const char *tree_path,
                                     char *buf, int buf_size)
{
    if (jce_editor_resolve_asset_path(tree_path, buf, buf_size) && buf[0])
        return buf;
    return tree_path;
}

/* Drop the edit-mode context (recycle).  Trees inside a JceBtContext are
 * never individually unloadable, so this is the only reclaim path. */
static void edit_ctx_reset(void)
{
    if (s.ctx) { jce_bt_destroy(s.ctx); s.ctx = nullptr; }
    s.tree        = JceBtTreeHandle{ UINT32_MAX };
    s.attempted   = false;
    s.load_failed = false;
}

/* Make sure the edit-mode context holds the tree for (entity, path).
 * Reloads only when entity / path / file mtime changed; both success
 * and failure are cached until then (no per-frame retry or log spam). */
static void edit_ctx_ensure(uint32_t entity_id, const char *tree_path)
{
    char host[1024];
    const char *resolved = resolve_tree_path(tree_path, host, (int)sizeof(host));

    int64_t mtime = 0;
    jce_fs_host_get_mtime(resolved, &mtime);   /* 0 when missing */

    if (s.attempted &&
        s.loaded_entity == entity_id &&
        std::strcmp(s.loaded_path, tree_path) == 0 &&
        s.loaded_mtime == mtime)
        return;   /* cached (loaded or failed) */

    edit_ctx_reset();
    s.attempted     = true;
    s.loaded_entity = entity_id;
    std::snprintf(s.loaded_path, sizeof(s.loaded_path), "%s", tree_path);
    s.loaded_mtime  = mtime;

    s.ctx = jce_bt_create();
    if (!s.ctx) { s.load_failed = true; return; }

    jce_bt_set_lenient_load(s.ctx, true);
    s.tree = jce_bt_load_tree_file(s.ctx, resolved);
    if (!jce_bt_tree_valid(s.tree))
        s.load_failed = true;
}

/* Indented node list with status dots.  `live` adds the observer ghost
 * ring (last completed result) and the transition counter. */
static void draw_tree(const JceBtContext *ctx, JceBtTreeHandle tree, bool live)
{
    const uint32_t count = jce_bt_node_count(ctx, tree);
    if (count == 0) {
        ImGui::TextDisabled("%s", jce_editor_i18n("btViz.loadFailed"));
        return;
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float row_h       = ImGui::GetTextLineHeightWithSpacing();
    const float radius      = ImGui::GetTextLineHeight() * 0.30f;
    const float indent_step = ImGui::GetTextLineHeight() * 1.1f;
    const float start_x     = ImGui::GetCursorPosX();
    const float dot_span    = radius * 2.0f + (live ? radius * 1.6f + 6.0f : 4.0f);

    for (uint32_t i = 0; i < count; ++i) {
        JceBtNodeInfo info;
        if (!jce_bt_node_info(ctx, tree, i, &info)) continue;

        ImGui::SetCursorPosX(start_x + (float)info.depth * indent_step);
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float cy = p.y + row_h * 0.5f;

        /* Live status dot. */
        dl->AddCircleFilled(ImVec2(p.x + radius, cy), radius,
                            status_color(info.status));
        /* Ghost dot: last completed result from the observer (live only). */
        if (live && info.last_result != JCE_BT_NODE_IDLE) {
            const float gx = p.x + radius * 2.0f + 4.0f + radius * 0.8f;
            ImU32 c = (status_color(info.last_result) & 0x00FFFFFFu) | 0xA0000000u;
            dl->AddCircleFilled(ImVec2(gx, cy), radius * 0.8f, c);
        }

        ImGui::Dummy(ImVec2(dot_span, row_h));
        ImGui::SameLine();

        const bool named = info.name[0] &&
                           std::strcmp(info.name, info.registration) != 0;
        if (named)
            ImGui::Text("%s  \"%s\"", info.registration, info.name);
        else
            ImGui::TextUnformatted(info.registration);

        if (live && info.transitions > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s: %u  [%s]",
                                jce_editor_i18n("btViz.transitions"),
                                info.transitions,
                                status_label(info.last_result));
        }

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s (uid %u)", info.registration, (unsigned)info.uid);
            ImGui::Text("%s", status_label(info.status));
            if (live)
                ImGui::Text("%s: %u", jce_editor_i18n("btViz.transitions"),
                            info.transitions);
            ImGui::EndTooltip();
        }
    }
}

void draw_content(void)
{
    const uint32_t focused = jce_state_get_focused();
    if (!focused) {
        ImGui::TextDisabled("%s", jce_editor_i18n("btViz.noSelection"));
        return;
    }

    JceScene *scene = jce_state_get_scene();
    JceEntity e = jce_state_to_ecs_entity(focused);
    JceBehaviorTree *bt = scene ? jce_scene_get_behavior_tree(scene, e) : nullptr;
    if (!bt) {
        ImGui::TextDisabled("%s", jce_editor_i18n("btViz.noComponent"));
        return;
    }
    if (!bt->tree_path[0]) {
        ImGui::TextDisabled("%s", jce_editor_i18n("btViz.noTree"));
        return;
    }

    /* Header: entity name + tree asset path. */
    ImGui::Text("%s", jce_state_entity_name(focused));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", bt->tree_path);
    ImGui::Separator();

    /* ── Play mode: poll the live runtime context ─────────────────── */
    const bool playing = jce_state_get_play_state() != JCE_PLAY_STOPPED;
    if (playing) {
        JceRuntime *rt = jce_editor_play_get_runtime();
        uint32_t tree_idx = UINT32_MAX;
        if (rt && jce_runtime_bt_tree(rt, (uint64_t)e, &tree_idx)) {
            JceBtContext *ctx = jce_runtime_bt_context(rt);
            JceBtTreeHandle h = { tree_idx };
            /* Idempotent: constructs the observer only on first call per
             * (runtime, tree); afterwards it is a cheap pointer check. */
            jce_bt_set_observed(ctx, h, true);
            draw_tree(ctx, h, true);
            return;
        }
        /* Entity has no loaded tree in the runtime (inactive / failed
         * load at Play start) — fall through to the structural view. */
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n("btViz.notLoadedRuntime"));
    }

    /* ── Edit mode: lenient structure-only preview ────────────────── */
    ImGui::TextDisabled("%s", jce_editor_i18n("btViz.editModeHint"));
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n_id("btViz.reload", "bt_viz")))
        edit_ctx_reset();   /* force a recycle next frame */

    edit_ctx_ensure(focused, bt->tree_path);

    if (s.load_failed || !s.ctx || !jce_bt_tree_valid(s.tree)) {
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.0f), "%s",
                           jce_editor_i18n("btViz.loadFailed"));
        return;
    }
    draw_tree(s.ctx, s.tree, false);
}

} /* namespace */

extern "C" void jce_editor_panel_bt_visualizer(void)
{
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###bt_visualizer",
             jce_editor_i18n("btViz.title"));
    if (!ImGui::Begin(_wt,
                      jce_editor_panel_visible_ptr(JCE_PANEL_BT_VISUALIZER),
                      ImGuiWindowFlags_NoFocusOnAppearing))
    {
        ImGui::End();
        return;
    }
    draw_content();
    ImGui::End();
}

extern "C" void jce_editor_panel_bt_visualizer_content(void)
{
    draw_content();
}
