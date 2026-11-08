/* Opt-in maintenance observations; no command server or alternate UI. */
#include "jce_editor_operation_metrics.h"
#include "jce_editor_state_internal.h"
#include "ui/jce_editor_panels.h"
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_scene_serial.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>

/* The existing state init/shutdown reset this first-use launch snapshot. */
static bool observe(void)
{
    if (!s.operation_metrics.selector_initialized) {
        s.operation_metrics.enabled = getenv("JCE_EDITOR_OPERATION_METRICS") != nullptr;
        s.operation_metrics.selector_initialized = true;
    }
    return s.operation_metrics.enabled;
}

void jce_editor_inspector_metric_request(uint32_t focused)
{
    if (!observe()) return;
    if (s.operation_metrics.pending)
        LOG_INFO("editor_operations", "inspector-request: generation=%llu status=coalesced",
                 (unsigned long long)s.operation_metrics.pending);
    s.operation_metrics.pending = ++s.operation_metrics.generation;
    s.operation_metrics.requested_entity = focused;
    s.operation_metrics.requested_at = jce_time_perf_counter();
    LOG_INFO("editor_operations", "inspector-request: generation=%llu entity=%u frame=%u",
             (unsigned long long)s.operation_metrics.pending, focused, s.operation_metrics.tick);
}

void jce_editor_inspector_metric_complete(uint32_t focused, bool valid, const char *display_name)
{
    if (observe()) {
        const char *requested = getenv("JCE_DBG_INSPECTOR_OBSERVE_AT");
        if (requested && !s.operation_metrics.observed) {
            char *end = nullptr;
            unsigned long frame = strtoul(requested, &end, 10);
            if (end && !*end && frame > 0 && frame == s.operation_metrics.tick) {
                s.operation_metrics.observed = true;
                // Called after actual Inspector content submission, independent
                // of whether a new synchronization generation was requested.
                LOG_INFO("editor_operations", "inspector-content-observation: frame=%u entity=%u valid=%d display_name=%s",
                         s.operation_metrics.tick, focused, valid ? 1 : 0, display_name ? display_name : "");
            }
        }
    }
    if (!s.operation_metrics.pending) return;
    double ms = jce_time_perf_to_ms(s.operation_metrics.requested_at, jce_time_perf_counter());
    bool match = valid && focused == s.operation_metrics.requested_entity;
    LOG_INFO("editor_operations", "inspector-complete: generation=%llu entity=%u frame=%u status=%s latency_ms=%.6f display_name=%s",
             (unsigned long long)s.operation_metrics.pending, focused, s.operation_metrics.tick, match ? "drawn" : "invalidated", ms, display_name ? display_name : "");
    if (match) jce_perf_phase_add("ed_inspector_response", ms);
    s.operation_metrics.pending = 0;
}

void jce_editor_hierarchy_metric(int loaded, int roots, int rows, int capacity)
{
    if (observe() && s.operation_metrics.tick == 50)
        LOG_INFO("editor_operations", "hierarchy-population: loaded=%d roots=%d rows=%d capacity=%d frame=%u",
                 loaded, roots, rows, capacity, s.operation_metrics.tick);
}

void jce_editor_hierarchy_reveal_metric(uint32_t entity, bool visible)
{
    if (observe()) LOG_INFO("editor_operations", "hierarchy-reveal: entity=%u visible=%d frame=%u",
                           entity, visible ? 1 : 0, s.operation_metrics.tick);
}

static uint32_t named_entity(const char *name)
{
    int n = jce_state_get_entity_count();
    for (int i = 0; i < n; ++i) {
        uint32_t id = jce_state_get_entity_id_by_index(i);
        const char *value = jce_state_entity_name(id);
        if (value && !strcmp(value, name)) return id;
    }
    return 0;
}

static void command(const char *key, bool parent)
{
    const char *value = getenv(key);
    if (!value) return;
    const char *at = strrchr(value, '@');
    if (!at || (unsigned)atoi(at + 1) != s.operation_metrics.tick) return;
    char source[128], target[128];
    const char *sep = strchr(value, ':');
    if (!sep || sep >= at || sep == value) {
        LOG_ERROR("editor_operations", "invalid %s specification", key);
        return;
    }
    size_t a = (size_t)(sep - value), b = (size_t)(at - sep - 1);
    if (a >= sizeof(source) || b >= sizeof(target)) {
        LOG_ERROR("editor_operations", "%s specification overflow", key);
        return;
    }
    memcpy(source, value, a); source[a] = 0;
    memcpy(target, sep + 1, b); target[b] = 0;
    uint32_t id = named_entity(source);
    uint32_t destination = parent && strcmp(target, "ROOT") ? named_entity(target) : 0;
    if (!id || (parent && strcmp(target, "ROOT") && !destination)) {
        LOG_ERROR("editor_operations", "%s entity missing", key);
        return;
    }
    if (!parent) {
        s.operation_metrics.renamed_entity = id;
        s.operation_metrics.renamed_parent = jce_state_entity_parent(id);
        s.operation_metrics.renamed_focus = jce_state_get_focused();
        s.operation_metrics.renamed_population = jce_state_get_entity_count();
        snprintf(s.operation_metrics.rename_before, sizeof(s.operation_metrics.rename_before), "%s", source);
        snprintf(s.operation_metrics.rename_after, sizeof(s.operation_metrics.rename_after), "%s", target);
    }
    if (parent) {
        const char *batch = getenv("JCE_DBG_REPARENT_BATCH");
        bool full_batch = batch && !strcmp(batch, "1");
        if (full_batch) jce_state_begin_batch_edit();
        jce_state_reparent_entity(id, destination);
        if (full_batch) jce_state_end_batch_edit();
    } else jce_state_rename_entity(id, target);
    const char *actual_name = jce_state_entity_name(id);
    uint32_t actual_parent = jce_state_entity_parent(id);
    bool correct = parent ? actual_parent == destination :
        (actual_name && !strcmp(actual_name, target));
    LOG_INFO("editor_operations", "editor-command: operation=%s entity=%u frame=%u correct=%d parent=%u name=%s",
             parent ? "reparent" : "rename", id, s.operation_metrics.tick, correct ? 1 : 0,
             actual_parent, actual_name ? actual_name : "");
    if (!correct) LOG_ERROR("editor_operations", "%s did not produce the requested state", key);
}

void jce_editor_operation_tick(void)
{
    ++s.operation_metrics.tick;
    command("JCE_DBG_RENAME", false);
    command("JCE_DBG_REPARENT", true);
    if (observe()) {
        const char *directory = getenv("JCE_DBG_SCENE_OBSERVE_DIR");
        const char *frames = getenv("JCE_DBG_SCENE_OBSERVE_AT");
        for (const char *cursor = frames; directory && cursor && *cursor;) {
            unsigned frame = 0; int consumed = 0;
            if (sscanf(cursor, "%u%n", &frame, &consumed) != 1 || consumed <= 0) break;
            if (frame == s.operation_metrics.tick) {
                JceScene *scene = jce_state_get_scene();
                char path[4096];
                int length = snprintf(path, sizeof(path), "%s/scene-%u.json", directory, s.operation_metrics.tick);
                size_t bytes = 0;
                char *json = jce_scene_serial_save(scene, &bytes);
                bool ok = json && length > 0 && (size_t)length < sizeof(path) &&
                          jce_fs_host_write_all(path, json, bytes);
                jce_scene_serial_free(json);
                if (ok) {
                    const JceEnvironmentState *environment = scene ? jce_scene_environment(scene) : nullptr;
                    if (environment)
                        jce_log_write(JCE_LOG_LEVEL_INFO, "editor_operations", __FILE__, __LINE__,
                            "scene-content-observation: frame=%u ok=1 bytes=%zu environment=available "
                            "world_time_seconds=%.17g weather_seed=%u", s.operation_metrics.tick, bytes,
                            environment->world_time_seconds, (unsigned)environment->weather_seed);
                    else
                        jce_log_write(JCE_LOG_LEVEL_INFO, "editor_operations", __FILE__, __LINE__,
                            "scene-content-observation: frame=%u ok=1 bytes=%zu environment=unavailable "
                            "world_time_seconds=unavailable weather_seed=unavailable", s.operation_metrics.tick, bytes);
                }
                if (!ok) LOG_ERROR("editor_operations", "scene observation serialization/write failed");
            }
            cursor += consumed;
            if (*cursor != ',') break;
            ++cursor;
        }
    }
    // Verify actual Undo/Redo results on the following tick, after the existing
    // maintenance hooks execute. This observer never restores state itself.
    const char *verify = getenv("JCE_DBG_RENAME_VERIFY");
    unsigned undo_tick = 0, redo_tick = 0;
    if (verify && sscanf(verify, "%u,%u", &undo_tick, &redo_tick) == 2 &&
        (s.operation_metrics.tick == undo_tick || s.operation_metrics.tick == redo_tick)) {
        const char *expected = s.operation_metrics.tick == undo_tick ? s.operation_metrics.rename_before : s.operation_metrics.rename_after;
        const char *actual = jce_state_entity_name(s.operation_metrics.renamed_entity);
        bool correct = s.operation_metrics.renamed_entity && actual && !strcmp(actual, expected) &&
            jce_state_entity_exists(s.operation_metrics.renamed_entity) &&
            jce_state_entity_parent(s.operation_metrics.renamed_entity) == s.operation_metrics.renamed_parent &&
            jce_state_get_entity_count() == s.operation_metrics.renamed_population &&
            jce_state_get_focused() == s.operation_metrics.renamed_focus;
        LOG_INFO("editor_operations", "rename-history: frame=%u phase=%s entity=%u correct=%d name=%s",
                 s.operation_metrics.tick, s.operation_metrics.tick == undo_tick ? "undo" : "redo", s.operation_metrics.renamed_entity,
                 correct ? 1 : 0, actual ? actual : "");
        if (!correct) LOG_ERROR("editor_operations", "rename history identity/state mismatch");
    }
}
