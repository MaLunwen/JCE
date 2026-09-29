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
#include "jce_editor_history_order.h"
#include "ui/jce_editor_panels.h"
#include "scene/jce_editor_scene_render.h"
#include "scene/jce_editor_game_render.h"

extern "C" {
#include <jce/os/core/jce_sysinfo.h>   /* machine-class undo budget */
#include <jce/middleware/scene/jce_component_registry.h>   /* generic comp id set */
#include <jce/middleware/scene/jce_scene_components_json.h> /* per-entity ser/parse */
}
#include <cstdlib>
#include <algorithm>
#include <array>

static std::vector<JceEditorHistoryProvider> s_history_providers;
static uint64_t s_history_next_sequence = 1u;
static constexpr size_t kHistoryProviderLimit = 8u;

static bool history_provider_registered(void *user)
{
    return std::any_of(
        s_history_providers.begin(), s_history_providers.end(),
        [user](const JceEditorHistoryProvider &provider) {
            return provider.user == user;
        });
}

static void history_clear_all_redo(void)
{
    s_redo_history.clear();
    for (const JceEditorHistoryProvider &provider : s_history_providers)
        provider.clear_redo(provider.user);
}

static uint64_t history_allocate_sequence(void)
{
    if (s_history_next_sequence == UINT64_MAX) {
        LOG_WARN(LOG_TAG, "history sequence exhausted; clearing history");
        jce_state_history_clear();
    }
    return s_history_next_sequence++;
}

bool jce_state_history_register_provider(
    const JceEditorHistoryProvider *provider)
{
    if (!provider || !provider->user ||
        !provider->peek_undo_sequence || !provider->peek_redo_sequence ||
        !provider->undo || !provider->redo || !provider->clear ||
        !provider->clear_redo)
        return false;

    for (JceEditorHistoryProvider &registered : s_history_providers) {
        if (registered.user == provider->user) {
            registered = *provider;
            return true;
        }
    }
    if (s_history_providers.size() >= kHistoryProviderLimit) {
        LOG_WARN(LOG_TAG, "history provider limit reached");
        return false;
    }
    s_history_providers.push_back(*provider);
    return true;
}

void jce_state_history_unregister_provider(void *user)
{
    auto it = std::find_if(
        s_history_providers.begin(), s_history_providers.end(),
        [user](const JceEditorHistoryProvider &provider) {
            return provider.user == user;
        });
    if (it == s_history_providers.end()) return;
    it->clear(it->user);
    s_history_providers.erase(it);
}

uint64_t jce_state_history_commit_external(void *user)
{
    if (!history_provider_registered(user)) {
        LOG_WARN(LOG_TAG, "history commit from unregistered provider");
        return 0u;
    }
    history_clear_all_redo();
    s.scene_modified = true;
    return history_allocate_sequence();
}

void jce_state_history_clear(void)
{
    s_undo_history.clear();
    s_redo_history.clear();
    for (const JceEditorHistoryProvider &provider : s_history_providers)
        provider.clear(provider.user);
    s_history_next_sequence = 1u;
}

/* ── History begin/end edit ───────────────────────────────────────── */

/* ── Is history PRODUCTION suspended right now? ──────────────────────
 *
 * Two reasons, and the second was missing for as long as Play existed.
 *
 * jce_state_undo/_redo refuse to run during Play, and their comment says the
 * mutation stream is "isolated from edit-mode history".  That was true of the
 * READ side only.  Nothing on the WRITE side checked play state, and
 * jce_editor_play.cpp never raises s_history_suspend_depth -- so every
 * inspector tweak and gizmo drag during a playtest serialised the LIVE
 * SIMULATED scene and pushed it onto s_undo_history.
 *
 * Two consequences, both silent.  The stack is capped
 * (JCE_UNDO_HISTORY_LIMIT) and evicts FIFO, so a few minutes of live tuning
 * discarded the session's real edit history.  And after Stop, those entries
 * are undoable: Ctrl+Z -- the reflex when you realise Stop reverted something
 * -- wrote physics-settled, script-mutated play state over the authored scene
 * and set scene_modified, so it looked like legitimate work.
 *
 * Play captures its own restore point directly (history_capture_snapshot into
 * s_play_snapshot, jce_editor_play.cpp:399) and never goes through the undo
 * stack, so refusing to produce here costs Play nothing. */
static bool history_production_suspended(void)
{
    return s_history_suspend_depth > 0 ||
           jce_state_get_play_state() != JCE_PLAY_STOPPED;
}

/* Entity the OUTERMOST open edit is scoped to, or 0 for a full-scene edit.
 * Only the outermost matters: a nested begin joins the record already pushed. */
static uint32_t s_history_edit_scope_entity = 0u;

bool history_begin_edit_scoped(uint32_t entity_id)
{
    if (history_production_suspended())
        return false;

    if (s_history_edit_nesting == 0) {
        s_history_edit_scope_entity = entity_id;
        s_history_outer_edit_pushed_snapshot =
            history_push_undo_snapshot_scoped(entity_id);
    }

    ++s_history_edit_nesting;
    return true;
}

bool history_begin_edit(void)
{
    return history_begin_edit_scoped(0u);
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

    /* Did anything actually change?  The unscoped path answers this by
     * serialising the WHOLE SCENE A SECOND TIME and comparing strings -- so a
     * slider release on a 50k-entity scene paid ~1.4 s and a 26 MB comparison,
     * half of it just to decide whether to keep the record it already had.
     * A scoped edit compares the one entity it touched. */
    bool changed = true;
    const uint32_t scope = s_history_edit_scope_entity;
    s_history_edit_scope_entity = 0u;

    if (scope != 0u && !s_undo_history.empty() &&
        s_undo_history.back().entity_id == scope) {
        EditorHistorySnapshot current;
        if (history_capture_entity_snapshot(scope, &current)) {
            const EditorHistorySnapshot &before = s_undo_history.back();
            changed = !(before.entity_json == current.entity_json
                        && before.entity_comp_ids == current.entity_comp_ids
                        && before.entity_order == current.entity_order);
        }
    } else {
        EditorHistorySnapshot current;
        if (history_capture_snapshot(&current) && !s_undo_history.empty()) {
            const EditorHistorySnapshot &before = s_undo_history.back();
            changed = !(before.scene_json == current.scene_json
                        && before.scene_path == current.scene_path
                        && before.component_orders == current.component_orders);
        }
    }

    if (changed) {
        if (s_history_outer_edit_pushed_snapshot && !s_undo_history.empty())
            s_undo_history.back().sequence = history_allocate_sequence();
        history_clear_all_redo();
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
    out->selection.assign(s.selected, s.selected + s.selected_count);
    out->focused = s.focused;
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

/* One entity's components, its component SET and its inspector order.
 *
 * The SET matters as much as the values: jce_scene_parse_entity_json only
 * ADDS and overwrites, so without it an undo could not remove a component the
 * edit had added -- it would restore the old values of everything else and
 * silently leave the new component behind. */
bool history_capture_entity_snapshot(uint32_t entity_id,
                                     EditorHistorySnapshot *out)
{
    if (!out || entity_id == 0u || !s.scene)
        return false;

    JceEntity e = jce_state_to_ecs_entity(entity_id);
    if (!e || !jce_scene_entity_alive(s.scene, e))
        return false;

    JceJson *comps = jce_scene_serialize_entity_components(s.scene, e);
    if (!comps)
        return false;

    /* Wrapped in an object with the key the parser reads, so restore hands it
     * straight back without rebuilding the shape. */
    JceJson *obj = jce_json_object();
    if (!obj) { jce_json_free(comps); return false; }
    jce_json_set_child(obj, "components", comps);  /* obj owns comps now */
    char *text = jce_json_print(obj, false);
    jce_json_free(obj);
    if (!text)
        return false;

    out->entity_id  = entity_id;
    out->entity_json = text;
    jce_json_free_string(text);
    out->scene_json.clear();
    out->scene_path = s.current_scene_path;

    out->entity_comp_ids.clear();
    const int comp_n = jce_component_count();
    for (int cid = 0; cid < comp_n; ++cid)
        if (jce_scene_has_comp(s.scene, e, cid))
            out->entity_comp_ids.push_back(cid);

    out->entity_order.clear();
    {
        auto it = g_entity_sidecar.find(entity_id);
        if (it != g_entity_sidecar.end())
            out->entity_order = it->second.component_order;
    }
    out->component_orders.clear();
    return true;
}

static bool history_restore_entity_snapshot(const EditorHistorySnapshot &snap,
                                            const char *reason)
{
    if (!s.scene) return false;

    JceEntity e = jce_state_to_ecs_entity(snap.entity_id);
    if (!e || !jce_scene_entity_alive(s.scene, e)) {
        /* REFUSED, not half-applied.  The caller has a rollback path; applying
         * an entity record to a scene where that entity no longer exists would
         * silently do nothing and report success. */
        LOG_WARN(LOG_TAG,
                 "history restore (%s): entity %u is gone; scoped record "
                 "cannot be applied", reason ? reason : "?", snap.entity_id);
        return false;
    }

    HistorySuspendScope suspend;

    /* Components the entity has NOW but the record does not: the edit added
     * them, so undo removes them.  Done before the parse so a re-added
     * component gets its recorded values rather than a merge of both. */
    const int comp_n = jce_component_count();
    for (int cid = 0; cid < comp_n; ++cid) {
        if (!jce_scene_has_comp(s.scene, e, cid))
            continue;
        bool in_record = false;
        for (int have : snap.entity_comp_ids)
            if (have == cid) { in_record = true; break; }
        if (!in_record)
            jce_scene_remove_comp(s.scene, e, cid);
    }

    JceJson *obj = jce_json_parse(snap.entity_json.c_str(),
                                  snap.entity_json.size());
    if (!obj) {
        LOG_WARN(LOG_TAG, "history restore (%s): entity record unparseable",
                 reason ? reason : "?");
        return false;
    }
    jce_scene_parse_entity_json(s.scene, e, obj);
    jce_json_free(obj);

    if (!snap.entity_order.empty())
        g_entity_sidecar[snap.entity_id].component_order = snap.entity_order;

    /* The selection, the entity handles and the occlusion cullers are all
     * untouched -- which is the other half of what this record buys.  A full
     * restore clears the scene and loses every one of them. */
    s.scene_modified = true;
    return true;
}

static size_t history_snapshot_bytes(const EditorHistorySnapshot &snap)
{
    size_t n = snap.scene_json.capacity() + snap.scene_path.capacity()
               + snap.entity_json.capacity()
               + snap.entity_comp_ids.capacity() * sizeof(int)
               + snap.entity_order.capacity() * sizeof(int)
               + snap.selection.capacity() * sizeof(uint32_t)
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

/* Push the record for an edit scoped to `entity_id`, or the full-scene record
 * when it is 0 -- or when the scoped capture cannot be made (the entity is
 * gone, the serialiser refuses).  Falling back to the full record is the safe
 * direction: an undo entry that is bigger than it needs to be still restores
 * correctly, while a missing one loses the user's work. */
bool history_push_undo_snapshot_scoped(uint32_t entity_id)
{
    if (entity_id == 0u)
        return history_push_undo_snapshot();

    EditorHistorySnapshot snap;
    if (!history_capture_entity_snapshot(entity_id, &snap))
        return history_push_undo_snapshot();

    snap.sequence = 0u;
    s_undo_history.push_back(std::move(snap));
    history_enforce_budget();
    return true;
}

bool history_push_undo_snapshot(void)
{
    if (history_production_suspended())
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
    /* Two kinds on one stack; entity_id != 0 is the scoped one. */
    if (snapshot.entity_id != 0u)
        return history_restore_entity_snapshot(snapshot, reason);

    if (snapshot.scene_json.empty())
        return false;

    HistorySuspendScope suspend;

    /* Keep the scene object and its shared mutable asset caches alive. Terrain
     * sculpt data is scene-owned but intentionally not embedded in scene JSON;
     * destroying JceScene here silently replaced unsaved edits with the disk
     * asset when the next frame reloaded it. jce_scene_clear() is the engine's
     * transactional-reload primitive: it removes user entities while retaining
     * the scene handle, component registry, and resource caches. */
    g_entity_order.clear();
    ++g_entity_order_gen;
    jce_roots_invalidate();
    g_entity_sidecar.clear();
    if (jce_scene_clear(s.scene) < 0) {
        LOG_WARN(LOG_TAG, "history restore could not clear the scene");
        return false;
    }
    jce_state_clear_selection();

    /* Reloaded entities receive fresh handles, so the occlusion cullers'
     * id-keyed slots now describe DEAD objects: a recreated entity
     * landing on a stale slot inherits its cull verdict (objects vanish
     * right after undo), and non-colliding ids allocate new bgfx queries
     * the dead slots never return (pool cap 256 -> exhausted on the first
     * undo in a dense scene).  Reset both viewport cullers.  Deliberately
     * NOT the full model-cache invalidation: model caches are path-keyed,
     * still valid across undo, and dropping them would add reload hitching
     * to every Ctrl+Z. */
    jce_editor_scene_render_reset_occlusion();
    jce_editor_game_render_reset_occlusion();

    /* Use the serializer's creation roster to remap editor IDs. Names may
     * repeat and recycled entity indices must never select another object. */
    JceJson *root = jce_json_parse(snapshot.scene_json.c_str(),
                                   snapshot.scene_json.size());
    const JceJson *entities = jce_json_get(jce_json_get(root, "scene"), "entities");
    JceSceneLoadStream *load = root
        ? jce_scene_load_stream_begin(s.scene, root, nullptr) : nullptr;
    std::unordered_map<uint32_t, uint32_t> remap;
    bool ok = false;
    if (load) {
        jce_scene_load_stream_step(load, 0);
        const uint32_t count = jce_scene_load_stream_new_entities(load, nullptr, 0);
        std::vector<JceEntity> created(count);
        jce_scene_load_stream_new_entities(load, created.data(), count);
        ok = jce_scene_load_stream_finalize(load) >= 0;
        if (count != (uint32_t)jce_json_array_size(entities)) ok = false;
        if (ok) {
            for (uint32_t i = 0; i < count; ++i) {
                const JceJson *item = jce_json_array_at(entities, (int)i);
                const uint32_t old_id = (uint32_t)(uint64_t)
                    jce_json_get_number(item, "id", 0);
                remap.emplace(old_id, (uint32_t)created[i]);
            }
        }
    }
    jce_json_free(root);
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
    for (const auto &kv : snapshot.component_orders) {
        const auto it = remap.find(kv.first);
        if (it != remap.end())
            g_entity_sidecar[it->second].component_order = kv.second;
    }
    for (uint32_t id : snapshot.selection) {
        const auto it = remap.find(id);
        if (it != remap.end()) jce_state_select_entity(it->second, true);
    }
    const auto focused = remap.find(snapshot.focused);
    if (focused != remap.end() && jce_state_is_selected(focused->second))
        jce_state_set_focused(focused->second);
    jce_editor_inspector_request_sync();

    if (!snapshot.scene_path.empty())
        set_current_scene_path_internal(snapshot.scene_path.c_str());

    return true;
}

/* ── Undo / Redo ─────────────────────────────────────────────────── */

void jce_state_undo(void)
{
    /* Play is a transient sandbox restored on Stop.  Its mutation stream is
     * kept out of edit-mode history on BOTH sides: production is refused by
     * history_production_suspended() above, and consumption here.  This half
     * used to stand alone, and its own claim of isolation was false because of
     * it -- see the note on that function. */
    if (jce_state_get_play_state() != JCE_PLAY_STOPPED) {
        LOG_INFO(LOG_TAG, "undo: ignored during Play mode (stop play first)");
        return;
    }

    std::array<uint64_t, kHistoryProviderLimit + 1u> sequences{};
    sequences[0] = s_undo_history.empty()
        ? 0u : s_undo_history.back().sequence;
    for (size_t i = 0; i < s_history_providers.size(); ++i)
        sequences[i + 1u] = s_history_providers[i].peek_undo_sequence(
            s_history_providers[i].user);
    const JceEditorHistorySelection selected =
        jce_editor_history_select_undo(sequences.data(),
                                       s_history_providers.size() + 1u);
    if (selected.index > 0u && selected.index <= s_history_providers.size()) {
        JceEditorHistoryProvider *selected_provider =
            &s_history_providers[selected.index - 1u];
        if (selected_provider->undo(selected_provider->user))
            LOG_INFO(LOG_TAG, "undo: applied external command");
        else
            LOG_WARN(LOG_TAG, "undo: external command failed");
        return;
    }

    if (selected.index != 0u) {
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
    current.sequence = target.sequence;

    if (!history_restore_snapshot(target, "undo")) {
        if (!history_restore_snapshot(current, "undo rollback"))
            LOG_ERROR(LOG_TAG, "undo: rollback failed after restore error");
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
    /* Keep Play-mode mutations isolated from edit-mode history. */
    if (jce_state_get_play_state() != JCE_PLAY_STOPPED) {
        LOG_INFO(LOG_TAG, "redo: ignored during Play mode (stop play first)");
        return;
    }

    std::array<uint64_t, kHistoryProviderLimit + 1u> sequences{};
    sequences[0] = s_redo_history.empty()
        ? 0u : s_redo_history.back().sequence;
    for (size_t i = 0; i < s_history_providers.size(); ++i)
        sequences[i + 1u] = s_history_providers[i].peek_redo_sequence(
            s_history_providers[i].user);
    const JceEditorHistorySelection selected =
        jce_editor_history_select_redo(sequences.data(),
                                       s_history_providers.size() + 1u);
    if (selected.index > 0u && selected.index <= s_history_providers.size()) {
        JceEditorHistoryProvider *selected_provider =
            &s_history_providers[selected.index - 1u];
        if (selected_provider->redo(selected_provider->user))
            LOG_INFO(LOG_TAG, "redo: applied external command");
        else
            LOG_WARN(LOG_TAG, "redo: external command failed");
        return;
    }

    if (selected.index != 0u) {
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
    current.sequence = target.sequence;

    if (!history_restore_snapshot(target, "redo")) {
        if (!history_restore_snapshot(current, "redo rollback"))
            LOG_ERROR(LOG_TAG, "redo: rollback failed after restore error");
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
    if (!s_undo_history.empty() && s_undo_history.back().sequence != 0u)
        return true;
    for (const JceEditorHistoryProvider &provider : s_history_providers)
        if (provider.peek_undo_sequence(provider.user) != 0u) return true;
    return false;
}

bool jce_state_can_redo(void)
{
    if (!s_redo_history.empty() && s_redo_history.back().sequence != 0u)
        return true;
    for (const JceEditorHistoryProvider &provider : s_history_providers)
        if (provider.peek_redo_sequence(provider.user) != 0u) return true;
    return false;
}

/* ── Batch edit scope ────────────────────────────────────────────── */

void jce_state_begin_batch_edit(void)
{
    if (history_begin_edit())
        ++s_history_manual_batch_depth;
}

void jce_state_begin_entity_edit(uint32_t entity_id)
{
    if (history_begin_edit_scoped(entity_id))
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
        history_clear_all_redo();
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
