/*
 * jce_editor_history.cpp  Undo/redo history and transaction management.
 *
 * Implements snapshot capture, history push/restore, and the public
 * undo/redo/batch-edit/transaction API.
 *
 * Snapshots use the engine serializer (jce_scene_serial_save/load)
 * directly against the ECS — no editor mirror store.
 */

#include "jce_editor_state_internal.h"
#include "scene/jce_editor_scene_render.h"
#include "scene/jce_editor_game_render.h"

extern "C" {
#include <jce/os/core/jce_sysinfo.h>   /* machine-class undo budget */
}
#include <cstdlib>

/* ── History begin/end edit ───────────────────────────────────────── */

bool history_begin_edit(void)
{
    if (s_history_suspend_depth > 0)
        return false;

    if (s_history_edit_nesting == 0)
        s_history_outer_edit_pushed_snapshot = history_push_undo_snapshot();

    ++s_history_edit_nesting;
    return true;
}

void history_end_edit(bool active)
{
    if (!active)
        return;
    if (s_history_edit_nesting <= 0)
        return;

    --s_history_edit_nesting;
    if (s_history_edit_nesting != 0)
        return;

    bool changed = true;
    EditorHistorySnapshot current;
    if (history_capture_snapshot(&current) && !s_undo_history.empty()) {
        const EditorHistorySnapshot &before = s_undo_history.back();
        changed = !(before.scene_json == current.scene_json
                    && before.scene_path == current.scene_path
                    && before.component_orders == current.component_orders);
    }

    if (changed) {
        s_redo_history.clear();
        s.scene_modified = true;
    } else if (s_history_outer_edit_pushed_snapshot && !s_undo_history.empty()) {
        s_undo_history.pop_back();
    }

    s_history_outer_edit_pushed_snapshot = false;
}

/* ── History snapshot capture ────────────────────────────────────── */

bool history_capture_snapshot(EditorHistorySnapshot *out)
{
    if (!out)
        return false;

    size_t json_len = 0;
    char *json_text = jce_scene_serial_save(s.scene, &json_len);
    if (!json_text)
        return false;

    out->scene_json = json_text;
    out->scene_path = s.current_scene_path;
    jce_json_free_string(json_text);

    /* Capture per-entity inspector component_order so reorder ops are
       undoable. Empty orders are skipped to keep snapshots small. */
    out->component_orders.clear();
    for (const auto &kv : g_entity_sidecar) {
        if (!kv.second.component_order.empty())
            out->component_orders.emplace(kv.first, kv.second.component_order);
    }
    return true;
}

static size_t history_snapshot_bytes(const EditorHistorySnapshot &snap)
{
    size_t n = snap.scene_json.capacity() + snap.scene_path.capacity()
               + sizeof(EditorHistorySnapshot);
    for (const auto &kv : snap.component_orders)
        n += sizeof(uint32_t) + kv.second.capacity() * sizeof(int);
    return n;
}

static size_t history_total_bytes(void)
{
    size_t total = 0;
    for (const auto &it : s_undo_history) total += history_snapshot_bytes(it);
    for (const auto &it : s_redo_history) total += history_snapshot_bytes(it);
    return total;
}

/* Undo budget by machine class (512MB charter): full-scene JSON snapshots at
 * 64MB are cheap insurance on a developer box but real money on a 512MB
 * machine — quarter the budget there (shorter undo depth on big scenes; the
 * count cap still applies first for typical scenes).  JCE_LOW_MEM=1/0
 * overrides, mirroring the renderer/allocator machine-class gates. */
static size_t history_budget_bytes(void)
{
    static size_t s_budget = 0;
    if (s_budget == 0) {
        bool lm = false;
        JceSysInfo si;
        jce_sysinfo_init(&si);
        lm = (si.ram_total_mb > 0 && si.ram_total_mb < 2048) || si.cpu_cores <= 1;
        const char *ev = getenv("JCE_LOW_MEM");
        if (ev && ev[0]) lm = (ev[0] != '0');
        s_budget = lm ? (JCE_UNDO_HISTORY_BYTES_BUDGET / 4u)
                      : JCE_UNDO_HISTORY_BYTES_BUDGET;
    }
    return s_budget;
}

static void history_enforce_budget(void)
{
    while (history_total_bytes() > history_budget_bytes()) {
        /* Drop oldest from whichever side is bigger; prefer redo first
         * since redo is rarely consulted compared to undo. */
        if (!s_redo_history.empty()
            && history_snapshot_bytes(s_redo_history.front())
                   >= history_snapshot_bytes(s_undo_history.empty()
                                                 ? s_redo_history.front()
                                                 : s_undo_history.front())) {
            s_redo_history.erase(s_redo_history.begin());
        } else if (!s_undo_history.empty()) {
            s_undo_history.erase(s_undo_history.begin());
        } else {
            break;
        }
    }
}

bool history_push_undo_snapshot(void)
{
    if (s_history_suspend_depth > 0)
        return false;

    EditorHistorySnapshot snap;
    if (!history_capture_snapshot(&snap))
        return false;

    if (!s_undo_history.empty()) {
        const EditorHistorySnapshot &last = s_undo_history.back();
        if (last.scene_json == snap.scene_json
            && last.scene_path == snap.scene_path
            && last.component_orders == snap.component_orders)
            return false;
    }

    if ((int)s_undo_history.size() >= JCE_UNDO_HISTORY_LIMIT)
        s_undo_history.erase(s_undo_history.begin());

    s_undo_history.push_back(std::move(snap));
    history_enforce_budget();
    return true;
}

bool history_restore_snapshot(const EditorHistorySnapshot &snapshot,
                              const char *reason)
{
    if (snapshot.scene_json.empty())
        return false;

    HistorySuspendScope suspend;

    /* Clear existing scene (destroys/recreates ECS world). */
    clear_scene_entities();

    /* The fresh flecs world restarts entity-id numbering, so the occlusion
     * cullers' id-keyed slots now describe DEAD objects: a recreated entity
     * landing on a stale slot inherits its cull verdict (objects vanish
     * right after undo), and non-colliding ids allocate new bgfx queries
     * the dead slots never return (pool cap 256 -> exhausted on the first
     * undo in a dense scene).  Reset both viewport cullers.  Deliberately
     * NOT the full model-cache invalidation: model caches are path-keyed,
     * still valid across undo, and dropping them would add reload hitching
     * to every Ctrl+Z. */
    jce_editor_scene_render_reset_occlusion();
    jce_editor_game_render_reset_occlusion();

    /* Load via engine serializer → ECS. */
    bool ok = jce_scene_serial_load(s.scene,
                                    snapshot.scene_json.c_str(),
                                    snapshot.scene_json.size());
    if (!ok) {
        LOG_WARN(LOG_TAG, "history restore failed (%s)",
                 reason ? reason : "unknown");
        return false;
    }

    /* Rebuild the editor's iteration order from the freshly-loaded ECS. */
    rebuild_entity_order_from_ecs();

    /* Restore Inspector component_order for entities we have data for.
       Sidecar entries without snapshot data keep current order; the next
       inspector pass will re-sync against new flag set. */
    for (auto &kv : g_entity_sidecar)
        kv.second.component_order.clear();
    for (const auto &kv : snapshot.component_orders)
        g_entity_sidecar[kv.first].component_order = kv.second;

    if (!snapshot.scene_path.empty())
        set_current_scene_path_internal(snapshot.scene_path.c_str());

    return true;
}

/* ── Undo / Redo ─────────────────────────────────────────────────── */

void jce_state_undo(void)
{
    /* Undo restores a snapshot via clear_scene_entities(), which destroys and
     * recreates s.scene.  During Play the runtime holds that scene pointer, so
     * swapping it out is a use-after-free.  Play is a transient sandbox (the
     * pre-play snapshot is restored on Stop), so undo here is void anyway —
     * mirror Unity and ignore scene undo while playing. */
    if (jce_state_get_play_state() != JCE_PLAY_STOPPED) {
        LOG_INFO(LOG_TAG, "undo: ignored during Play mode (stop play first)");
        return;
    }

    if (s_undo_history.empty()) {
        LOG_INFO(LOG_TAG, "undo: history empty");
        return;
    }

    EditorHistorySnapshot current;
    if (!history_capture_snapshot(&current)) {
        LOG_WARN(LOG_TAG, "undo: failed to capture current state");
        return;
    }

    EditorHistorySnapshot target = s_undo_history.back();
    s_undo_history.pop_back();

    if (!history_restore_snapshot(target, "undo")) {
        s_undo_history.push_back(std::move(target));
        LOG_WARN(LOG_TAG, "undo: restore failed");
        return;
    }

    if ((int)s_redo_history.size() >= JCE_UNDO_HISTORY_LIMIT)
        s_redo_history.erase(s_redo_history.begin());
    s_redo_history.push_back(std::move(current));
    history_enforce_budget();

    LOG_INFO(LOG_TAG, "undo: applied");
}

void jce_state_redo(void)
{
    /* See jce_state_undo — scene restore is unsafe while the runtime holds the
     * scene pointer during Play. */
    if (jce_state_get_play_state() != JCE_PLAY_STOPPED) {
        LOG_INFO(LOG_TAG, "redo: ignored during Play mode (stop play first)");
        return;
    }

    if (s_redo_history.empty()) {
        LOG_INFO(LOG_TAG, "redo: history empty");
        return;
    }

    EditorHistorySnapshot current;
    if (!history_capture_snapshot(&current)) {
        LOG_WARN(LOG_TAG, "redo: failed to capture current state");
        return;
    }

    EditorHistorySnapshot target = s_redo_history.back();
    s_redo_history.pop_back();

    if (!history_restore_snapshot(target, "redo")) {
        s_redo_history.push_back(std::move(target));
        LOG_WARN(LOG_TAG, "redo: restore failed");
        return;
    }

    if ((int)s_undo_history.size() >= JCE_UNDO_HISTORY_LIMIT)
        s_undo_history.erase(s_undo_history.begin());
    s_undo_history.push_back(std::move(current));
    history_enforce_budget();

    LOG_INFO(LOG_TAG, "redo: applied");
}

bool jce_state_can_undo(void)
{
    return !s_undo_history.empty();
}

bool jce_state_can_redo(void)
{
    return !s_redo_history.empty();
}

/* ── Batch edit scope ────────────────────────────────────────────── */

void jce_state_begin_batch_edit(void)
{
    if (history_begin_edit())
        ++s_history_manual_batch_depth;
}

void jce_state_end_batch_edit(void)
{
    if (s_history_manual_batch_depth <= 0)
        return;

    --s_history_manual_batch_depth;
    history_end_edit(true);
}

void jce_state_begin_transient_edit(void)
{
    ++s_history_suspend_depth;
    ++s_history_transient_batch_depth;
}

void jce_state_end_transient_edit(void)
{
    if (s_history_transient_batch_depth <= 0)
        return;

    --s_history_transient_batch_depth;
    if (s_history_suspend_depth > 0)
        --s_history_suspend_depth;

    if (s_history_transient_batch_depth == 0) {
        s_redo_history.clear();
        s.scene_modified = true;
    }
}

/* ── Explicit transaction scope ──────────────────────────────────── */

bool jce_state_begin_transaction(const char *label)
{
    if (s_transaction.active)
        return false;

    if (!history_capture_snapshot(&s_transaction.before))
        return false;

    if (label && label[0] != '\0')
        snprintf(s_transaction.label, sizeof(s_transaction.label), "%s", label);
    else
        snprintf(s_transaction.label, sizeof(s_transaction.label), "transaction");

    int depth_before = s_history_manual_batch_depth;
    jce_state_begin_batch_edit();
    if (s_history_manual_batch_depth == depth_before) {
        s_transaction.before.scene_json.clear();
        s_transaction.before.scene_path.clear();
        s_transaction.label[0] = '\0';
        return false;
    }

    s_transaction.active = true;
    return true;
}

void jce_state_commit_transaction(void)
{
    if (!s_transaction.active)
        return;

    jce_state_end_batch_edit();
    s_transaction.active = false;
    s_transaction.before.scene_json.clear();
    s_transaction.before.scene_path.clear();
    s_transaction.label[0] = '\0';
}

void jce_state_cancel_transaction(void)
{
    if (!s_transaction.active)
        return;

    history_restore_snapshot(s_transaction.before,
                             s_transaction.label[0] ? s_transaction.label : "transaction");
    jce_state_end_batch_edit();

    s_transaction.active = false;
    s_transaction.before.scene_json.clear();
    s_transaction.before.scene_path.clear();
    s_transaction.label[0] = '\0';
}

bool jce_state_transaction_active(void)
{
    return s_transaction.active;
}
