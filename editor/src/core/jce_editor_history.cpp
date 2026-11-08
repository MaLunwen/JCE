#include "jce_editor_history_identity.h"
#include "jce_editor_history_entity.h"
#include "jce_editor_operation_metrics.h"
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
#include <cstring>
#include <jce/os/core/jce_hash.h>
#include <algorithm>
#include <array>
#include <unordered_set>

static std::vector<JceEditorHistoryProvider> s_history_providers;
static uint64_t s_history_next_sequence = 1u;
static constexpr size_t kHistoryProviderLimit = 8u;

/* Tokens are private history-session identities, never names or serialized
 * public IDs. Exact ECS handles prevent recycled indices inheriting a token. */
static JceEditorHistoryIdentity s_history_identity;

static uint64_t history_entity_token(uint32_t editor_id)
{
    if (!s.scene) return 0u;
    JceEntity entity = jce_state_to_ecs_entity(editor_id);
    return entity && jce_scene_entity_alive(s.scene, entity)
        ? s_history_identity.token(entity) : 0u;
}

static uint32_t history_resolve_token(uint64_t token)
{
    JceEntity entity = s_history_identity.handle(token);
    if (!s.scene || !entity || !jce_scene_entity_alive(s.scene, entity)) return 0u;
    uint32_t id = (uint32_t)entity;
    return jce_state_to_ecs_entity(id) == entity ? id : 0u;
}

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
    s_history_identity.clear();
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

bool history_entity_edit_compatible(uint32_t entity_id)
{
    return s_history_edit_nesting == 0 || s_history_edit_scope_entity == 0u ||
           s_history_edit_scope_entity == entity_id;
}

bool history_begin_edit_scoped(uint32_t entity_id)
{
    if (history_production_suspended())
        return false;
    if (!history_entity_edit_compatible(entity_id)) {
        LOG_ERROR(LOG_TAG, "cross-entity nested scoped history is unsupported; use an outer full-scene batch");
        return false;
    }

    if (s_history_edit_nesting == 0) {
        s_history_edit_scope_entity = entity_id;
        s_history_outer_edit_pushed_snapshot =
            history_push_undo_snapshot_scoped(entity_id);
        if (!s_history_outer_edit_pushed_snapshot) { s_history_edit_scope_entity = 0; return false; }
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
        s_undo_history.back().entity_token == history_entity_token(scope)) {
        EditorHistorySnapshot current;
        if (history_capture_entity_snapshot(scope, &current)) {
            const EditorHistorySnapshot &before = s_undo_history.back();
            changed = !(before.entity_json == current.entity_json
                        && before.entity_comp_ids == current.entity_comp_ids
                        && before.entity_order == current.entity_order
                        && before.entity_parent_captured == current.entity_parent_captured
                        && before.entity_parent_token == current.entity_parent_token);
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

    s_history_identity.prune([](uint64_t handle) {
        return s.scene && jce_scene_entity_alive(s.scene, handle);
    });
    out->identity_epoch = s_history_identity.epoch();
    out->entity_tokens.clear();
    JceJson *identity_root = jce_json_parse(json_text, json_len);
    const JceJson *identity_entities = jce_json_get(jce_json_get(identity_root, "scene"), "entities");
    int identity_count = jce_json_array_size(identity_entities);
    bool identities_ok = identity_root && identity_entities;
    const JceJson *identity_item = jce_json_first_child(identity_entities);
    for (int i = 0; identities_ok && i < identity_count; ++i,
         identity_item = jce_json_next_sibling(identity_item)) {
        const JceJson *item = identity_item;
        if (!item) { identities_ok = false; break; }
        uint32_t id = (uint32_t)(uint64_t)jce_json_get_number(item, "id", 0);
        JceEntity handle = jce_state_to_ecs_entity(id);
        if (!handle || !jce_scene_entity_alive(s.scene, handle)) identities_ok = false;
        else {
            uint64_t token = s_history_identity.existing_token(handle);
            if (token) out->entity_tokens.emplace_back(id, token);
        }
    }
    jce_json_free(identity_root);
    if (!identities_ok) { jce_json_free_string(json_text); return false; }
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

    out->identity_epoch = s_history_identity.epoch();
    out->entity_id  = entity_id;
    out->entity_token = history_entity_token(entity_id);
    if (!out->entity_token) { jce_json_free_string(text); return false; }
    JceEntity parent = jce_scene_get_parent(s.scene, e);
    out->entity_parent_captured = true;
    out->entity_parent_token = parent ? history_entity_token((uint32_t)parent) : 0;
    if (parent && !out->entity_parent_token) { jce_json_free_string(text); return false; }
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

    if (!snap.identity_epoch || snap.identity_epoch != s_history_identity.epoch()) {
        LOG_WARN(LOG_TAG, "scoped history belongs to a different scene-history epoch");
        return false;
    }
    const uint32_t resolved_id = history_resolve_token(snap.entity_token);
    JceEntity e = resolved_id ? jce_state_to_ecs_entity(resolved_id) : JCE_ENTITY_INVALID;
    if (!e || !jce_scene_entity_alive(s.scene, e)) {
        /* REFUSED, not half-applied.  The caller has a rollback path; applying
         * an entity record to a scene where that entity no longer exists would
         * silently do nothing and report success. */
        LOG_WARN(LOG_TAG,
                 "history restore (%s): entity %u is gone; scoped record "
                 "cannot be applied", reason ? reason : "?", snap.entity_id);
        return false;
    }

    uint32_t parent_id = snap.entity_parent_token ? history_resolve_token(snap.entity_parent_token) : 0;
    if (snap.entity_parent_captured && snap.entity_parent_token && !parent_id) {
        LOG_WARN(LOG_TAG, "scoped history parent identity is unavailable");
        return false;
    }
    JceEntity parent = parent_id ? jce_state_to_ecs_entity(parent_id) : JCE_ENTITY_INVALID;
    if (snap.entity_parent_captured && snap.entity_parent_token &&
        (!parent || !jce_scene_entity_alive(s.scene, parent))) {
        LOG_WARN(LOG_TAG, "scoped history resolved parent handle is unavailable");
        return false;
    }
    HistorySuspendScope suspend;
    if (!jce_history_restore_entity(s.scene, e, parent, snap.entity_parent_captured,
                                   (size_t)jce_state_get_entity_count(), snap.entity_json,
                                   snap.entity_comp_ids, snap.entity_parent_token != 0)) {
        LOG_WARN(LOG_TAG, "history restore (%s): scoped relation/components rejected", reason ? reason : "?");
        return false;
    }
    if (snap.entity_parent_captured) jce_roots_note_reparented(resolved_id);

    if (!snap.entity_order.empty())
        g_entity_sidecar[resolved_id].component_order = snap.entity_order;

    if (jce_state_get_focused() == resolved_id)
        jce_editor_inspector_request_sync();

    LOG_INFO(LOG_TAG, "history-entity-restore: token=%llu source_id=%u current_id=%u reason=%s name=%s parent=%u",
             (unsigned long long)snap.entity_token, snap.entity_id, resolved_id,
             reason ? reason : "unknown", jce_state_entity_name(resolved_id),
             jce_state_entity_parent(resolved_id));

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
               + snap.entity_tokens.capacity() * sizeof(std::pair<uint32_t, uint64_t>)
               + sizeof(EditorHistorySnapshot);
    for (const auto &kv : snap.component_orders)
        n += sizeof(uint32_t) + kv.second.capacity() * sizeof(int);
    return n;
}

static size_t history_total_bytes(void)
{
    size_t total = s_history_identity.estimated_bytes();
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
                              const char *reason, bool *mutated)
{
    if (mutated) *mutated = false;
    /* Two kinds on one stack; entity_id != 0 is the scoped one. */
    if (snapshot.entity_id != 0u) {
        // Scoped component parsing is not transactional; rollback conservatively.
        if (mutated) *mutated = true;
        return history_restore_entity_snapshot(snapshot, reason);
    }

    if (snapshot.scene_json.empty())
        return false;

    // Validate snapshot identity before clearing any live scene. Names are not
    // identity, and duplicate or unknown source IDs cannot be guessed back.
    JceJson *root = jce_json_parse(snapshot.scene_json.c_str(), snapshot.scene_json.size());
    const JceJson *entities = jce_json_get(jce_json_get(root, "scene"), "entities");
    bool identity_valid = root && entities;
    try {
    if (identity_valid && snapshot.identity_epoch != 0u) {
        std::unordered_map<uint32_t, uint64_t> roster;
        std::unordered_set<uint64_t> tokens;
        for (const auto &entry : snapshot.entity_tokens)
            if (!entry.first || !entry.second || !roster.emplace(entry).second ||
                !tokens.insert(entry.second).second) identity_valid = false;
        int count = jce_json_array_size(entities);
        std::unordered_set<uint32_t> source_ids;
        source_ids.reserve((size_t)count);
        const JceJson *item = jce_json_first_child(entities);
        for (int i = 0; identity_valid && i < count; ++i, item = jce_json_next_sibling(item)) {
            if (!item) { identity_valid = false; break; }
            uint32_t id = (uint32_t)(uint64_t)jce_json_get_number(item, "id", 0);
            if (!id || !source_ids.insert(id).second) identity_valid = false;
            roster.erase(id);
        }
        if (!roster.empty()) identity_valid = false;
    }
    } catch (const std::bad_alloc &) {
        LOG_ERROR(LOG_TAG, "history roster validation allocation failed before scene clear");
        jce_json_free(root); return false;
    }
    if (!identity_valid) { jce_json_free(root); return false; }

    // Prepare identity nodes and remap storage before touching the live scene.
    JceEditorHistoryIdentity::Prepared prepared_identity;
    std::unordered_map<uint32_t, uint32_t> remap;
    std::unordered_map<uint32_t, JceEntity> remap_full;
    std::vector<JceEntity> created;
    std::vector<uint64_t> identity_handles;
    try {
        bool budget_probe = false;
        const char *metrics = getenv("JCE_EDITOR_OPERATION_METRICS");
        const char *budget_text = getenv("JCE_EDITOR_HISTORY_IDENTITY_BUDGET_BYTES");
        if (metrics && strcmp(metrics, "1") == 0 && budget_text) {
            char *end = nullptr;
            unsigned long long value = strtoull(budget_text, &end, 10);
            if (!end || *end || !s_history_identity.set_budget((size_t)value)) {
                LOG_ERROR(LOG_TAG, "invalid diagnostic history identity budget");
                jce_json_free(root); return false;
            }
            budget_probe = true;
        }
        size_t before_len = 0;
        char *before_text = budget_probe ? jce_scene_serial_save(s.scene, &before_len) : nullptr;
        std::string before_json;
        try { if (before_text) before_json.assign(before_text, before_len); }
        catch (...) { jce_json_free_string(before_text); throw; }
        jce_json_free_string(before_text);
        std::vector<uint32_t> before_selection;
        if (budget_probe) before_selection.assign(s.selected, s.selected + s.selected_count);
        const uint32_t before_focus = s.focused;
        const size_t count = (size_t)jce_json_array_size(entities);
        std::vector<std::pair<uint64_t, uint64_t>> roster;
        roster.reserve(snapshot.entity_tokens.size()); created.resize(count); identity_handles.resize(snapshot.entity_tokens.size());
        remap.reserve(count); remap_full.reserve(count);
        const JceJson *item = jce_json_first_child(entities);
        for (size_t i = 0; i < count; ++i, item = jce_json_next_sibling(item)) {
            if (!item) { jce_json_free(root); return false; }
            uint32_t source = (uint32_t)(uint64_t)jce_json_get_number(item, "id", 0);
            remap.emplace(source, 0); remap_full.emplace(source, 0);
        }
        for (const auto &entry : snapshot.entity_tokens) roster.emplace_back(entry.first, entry.second);
        if (!s_history_identity.prepare(roster, snapshot.identity_epoch, prepared_identity)) {
            if (budget_probe) {
                size_t after_len = 0;
                char *after = jce_scene_serial_save(s.scene, &after_len);
                const bool same_scene = after && before_len > 0 && before_json.size() == after_len &&
                    memcmp(before_json.data(), after, after_len) == 0;
                const bool same_selection = before_selection.size() == (size_t)s.selected_count &&
                    std::equal(before_selection.begin(), before_selection.end(), s.selected);
                LOG_INFO(LOG_TAG, "history-budget-refusal: unchanged=%d selected_unchanged=%d focused_unchanged=%d population=%d before_hash=%llu after_hash=%llu",
                    same_scene ? 1 : 0, same_selection ? 1 : 0, before_focus == s.focused ? 1 : 0,
                    jce_state_get_entity_count(),
                    (unsigned long long)jce_fnv1a64(before_json.data(), before_json.size()),
                    (unsigned long long)(after ? jce_fnv1a64(after, after_len) : 0));
                jce_json_free_string(after);
            }
            LOG_ERROR(LOG_TAG, "history identity preparation refused before scene clear");
            jce_json_free(root); return false;
        }
    } catch (const std::bad_alloc &) {
        LOG_ERROR(LOG_TAG, "history restore storage allocation failed before scene clear");
        jce_json_free(root); return false;
    }

    HistorySuspendScope suspend;

    /* Keep the scene object and its shared mutable asset caches alive. Terrain
     * sculpt data is scene-owned but intentionally not embedded in scene JSON;
     * destroying JceScene here silently replaced unsaved edits with the disk
     * asset when the next frame reloaded it. jce_scene_clear() is the engine's
     * transactional-reload primitive: it removes user entities while retaining
     * the scene handle, component registry, and resource caches. */
    jce_editor_scene_render_cancel_probe_capture();
    if (mutated) *mutated = true;
    g_entity_order.clear();
    ++g_entity_order_gen;
    jce_roots_invalidate();
    g_entity_sidecar.clear();
    if (jce_scene_clear(s.scene) < 0) {
        LOG_WARN(LOG_TAG, "history restore could not clear the scene");
        jce_json_free(root);
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
    JceSceneLoadStream *load = root
        ? jce_scene_load_stream_begin(s.scene, root, nullptr) : nullptr;
    bool ok = false;
    if (load) {
        jce_scene_load_stream_step(load, 0);
        const uint32_t count = jce_scene_load_stream_new_entities(load, nullptr, 0);
        if (count <= created.size()) jce_scene_load_stream_new_entities(load, created.data(), count);
        ok = jce_scene_load_stream_finalize(load) >= 0;
        if (count != (uint32_t)jce_json_array_size(entities)) ok = false;
        if (ok) {
            const JceJson *item = jce_json_first_child(entities);
            for (uint32_t i = 0; i < count; ++i, item = jce_json_next_sibling(item)) {
                if (!item) { ok = false; break; }
                const uint32_t old_id = (uint32_t)(uint64_t)
                    jce_json_get_number(item, "id", 0);
                remap.at(old_id) = (uint32_t)created[i];
                remap_full.at(old_id) = created[i];
            }
        }
    }
    jce_json_free(root);
    if (!ok) {
        LOG_WARN(LOG_TAG, "history restore failed (%s)",
                 reason ? reason : "unknown");
        return false;
    }

    for (size_t i = 0; i < prepared_identity.source_ids.size(); ++i)
        identity_handles[i] = remap_full.at((uint32_t)prepared_identity.source_ids[i]);
    if (!s_history_identity.commit(prepared_identity, identity_handles)) {
        LOG_ERROR(LOG_TAG, "history identity commit rejected loader roster");
        return false;
    }

    LOG_INFO(LOG_TAG, "history-identity: tracked=%zu population=%zu epoch=%llu",
             s_history_identity.count(), remap_full.size(),
             (unsigned long long)s_history_identity.epoch());

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
    uint32_t focus_id = jce_state_get_focused();
    uint32_t parent_id = focus_id ? jce_state_entity_parent(focus_id) : 0u;
    LOG_INFO(LOG_TAG, "history-scene-restore: reason=%s focused=%u token=%llu name=%s parent_name=%s population=%d",
             reason ? reason : "unknown", focus_id,
             (unsigned long long)(focus_id ? s_history_identity.existing_token(jce_state_to_ecs_entity(focus_id)) : 0u),
             focus_id ? jce_state_entity_name(focus_id) : "NONE",
             parent_id ? jce_state_entity_name(parent_id) : "ROOT", jce_state_get_entity_count());

    if (!snapshot.scene_path.empty())
        set_current_scene_path_internal(snapshot.scene_path.c_str());

    return true;
}

/* ── Undo / Redo ─────────────────────────────────────────────────── */

void jce_state_undo(void)
{
    JceEditorOperationScope operation_scope("ed_undo");
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

    // The inverse record must retain the target's granularity. Capturing a
    // full scene for an entity edit makes the next traversal recreate every
    // handle and defeats scoped history identity and cost guarantees.
    EditorHistorySnapshot target = s_undo_history.back();
    EditorHistorySnapshot current;
    const bool captured = target.entity_id != 0u
        ? (target.identity_epoch == s_history_identity.epoch() &&
           history_capture_entity_snapshot(history_resolve_token(target.entity_token), &current))
        : history_capture_snapshot(&current);
    if (!captured) {
        LOG_WARN(LOG_TAG, "undo: failed to capture current state");
        return;
    }
    s_undo_history.pop_back();
    current.sequence = target.sequence;

    bool restore_mutated = false;
    if (!history_restore_snapshot(target, "undo", &restore_mutated)) {
        if (restore_mutated && !history_restore_snapshot(current, "undo rollback"))
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
    JceEditorOperationScope operation_scope("ed_redo");
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

    // The inverse record must retain the target's granularity. Capturing a
    // full scene for an entity edit makes the next traversal recreate every
    // handle and defeats scoped history identity and cost guarantees.
    EditorHistorySnapshot target = s_redo_history.back();
    EditorHistorySnapshot current;
    const bool captured = target.entity_id != 0u
        ? (target.identity_epoch == s_history_identity.epoch() &&
           history_capture_entity_snapshot(history_resolve_token(target.entity_token), &current))
        : history_capture_snapshot(&current);
    if (!captured) {
        LOG_WARN(LOG_TAG, "redo: failed to capture current state");
        return;
    }
    s_redo_history.pop_back();
    current.sequence = target.sequence;

    bool restore_mutated = false;
    if (!history_restore_snapshot(target, "redo", &restore_mutated)) {
        if (restore_mutated && !history_restore_snapshot(current, "redo rollback"))
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
