/* Actual diagnostic owner over real private model slots; CPU evidence only. */
#include "unity.h"
#include "middleware/scene/jce_sr_internal.h"
#include "middleware/scene/jce_sr_model_diagnostics.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_log.h>
#include <string.h>
#include <stdio.h>

/* Exact production destroy admission prefix, before any resource retirement. */
#include JCE_MODEL_DIAGNOSTICS_DESTROY

enum { MODEL_DIAGNOSTICS_FIXTURE_CAPACITY = 256u };

static JceSceneRenderer *owner;
static JceSceneRenderer *other_owner;
static unsigned warnings;
static uint64_t occupancy_reads;

void jce_test_model_slot_scanned(void)
{
    ++occupancy_reads;
}
static char last_warning[1536];

static void observe(const JceLogRecord *record, void *user)
{
    (void)user;
    if (record->level != JCE_LOG_LEVEL_WARN ||
        !strstr(record->message, "model cache rejected")) return;
    ++warnings;
    snprintf(last_warning, sizeof(last_warning), "%s", record->message);
}

/* Fixture storage follows the actual pointer table. Dummy model tokens have
 * no GPU ownership and must never reach the production model destructor. */
static void fixture_table_create(JceSceneRenderer *sr)
{
    sr->model_cache = JCE_CALLOC(MODEL_DIAGNOSTICS_FIXTURE_CAPACITY,
        sizeof(*sr->model_cache));
    TEST_ASSERT_NOT_NULL(sr->model_cache);
    sr->model_cache_capacity = MODEL_DIAGNOSTICS_FIXTURE_CAPACITY;
}

static SrModelCache *fixture_entry(JceSceneRenderer *sr, uint32_t index)
{
    TEST_ASSERT_LESS_THAN_UINT32(sr->model_cache_capacity, index);
    TEST_ASSERT_NULL(sr->model_cache[index]);
    sr->model_cache[index] = JCE_CALLOC(1, sizeof(*sr->model_cache[index]));
    TEST_ASSERT_NOT_NULL(sr->model_cache[index]);
    sr->model_cache[index]->used = true;
    ++sr->model_cache_used;
    return sr->model_cache[index];
}

static void fixture_table_destroy(JceSceneRenderer *sr)
{
    if (sr->model_cache) for (uint32_t i = 0; i < sr->model_cache_capacity; ++i)
        JCE_FREE(sr->model_cache[i]);
    JCE_FREE(sr->model_cache);
    sr->model_cache = NULL;
    sr->model_cache_capacity = sr->model_cache_used = 0;
    sr->model_lookup_hint = NULL;
}

static void fixture_renderer_destroy(JceSceneRenderer **renderer)
{
    if (!*renderer) return;
    fixture_table_destroy(*renderer);
    JCE_FREE(*renderer);
    *renderer = NULL;
}

void setUp(void)
{
    owner = JCE_CALLOC(1, sizeof(*owner));
    TEST_ASSERT_NOT_NULL(owner);
    fixture_table_create(owner);
    warnings = 0;
    occupancy_reads = 0;
    last_warning[0] = '\0';
    jce_log_set_level(JCE_LOG_LEVEL_WARN);
    jce_log_set_sink(observe, NULL);
    jce_trace_set_enabled(true);
    jce_trace_reset();
}

void tearDown(void)
{
    jce_log_flush();
    jce_log_set_sink(NULL, NULL);
    jce_trace_set_enabled(false);
    jce_trace_reset();
    /* Both owners remain reachable after Unity longjmp, including failed
     * negative-control assertions and partial fixture allocation. */
    fixture_renderer_destroy(&other_owner);
    fixture_renderer_destroy(&owner);
}

static void all_occupied_states_count_and_warning_is_bounded(void)
{
    for (uint32_t i = 0; i < owner->model_cache_capacity; ++i) {
        SrModelCache *entry = fixture_entry(owner, i);
        if (i < 128u) entry->model = (JceModel *)owner;
        else if (i < 192u) entry->pending = true;
        else entry->failed = true;
    }
    sr_model_cache_failure(owner, "D:/private/project/secret.glb", 77,
        SR_MODEL_FAILURE_ENTRY_BUDGET);
    sr_model_cache_failure(owner, "D:/private/project/other.glb", 88,
        SR_MODEL_FAILURE_ENTRY_BUDGET);
    TEST_ASSERT_EQUAL_UINT(1, warnings);
    TEST_ASSERT_NOT_NULL(strstr(last_warning, "first_reason=entry_budget"));
    TEST_ASSERT_NOT_NULL(strstr(last_warning, "entity=77"));
    TEST_ASSERT_NOT_NULL(strstr(last_warning, "occupied=256 limit=256"));
    TEST_ASSERT_NOT_NULL(strstr(last_warning, "loaded=128 pending=64 failed=64 unavailable=0"));
    TEST_ASSERT_NULL(strstr(last_warning, "private/project"));
    TEST_ASSERT_NOT_NULL(strstr(last_warning, "path_hash="));
}

static void repeated_rejection_is_bounded_without_repeated_occupancy_walks(void)
{
    const char *path = "private/asset.glb";
    for (unsigned i = 0; i < 4096u; ++i)
        sr_model_cache_failure(owner, path, 71u + i, SR_MODEL_FAILURE_ENTRY_BUDGET);
    TEST_ASSERT_EQUAL_UINT64(MODEL_DIAGNOSTICS_FIXTURE_CAPACITY, occupancy_reads);
    JceTraceCursor cursor;
    JceTraceEvent events[128];
    jce_trace_cursor_init(&cursor);
    const uint32_t count = jce_trace_read(&cursor, events, 128);
    unsigned failures = 0, hashes = 0, entities = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const JceTraceEvent *event = &events[i];
        if (strcmp(event->name, "sr.model.cache.reject") == 0 &&
            event->type == JCE_TRACE_EVENT_TASK_END) {
            TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_FAILED, event->value_u32[0]);
            ++failures;
        }
        if (strcmp(event->name, "model.cache.path_hash") == 0) {
            TEST_ASSERT_EQUAL_UINT64(jce_fnv1a32_str(path), event->value_u64);
            TEST_ASSERT_NOT_EQUAL_UINT64(0, event->id);
            TEST_ASSERT_EQUAL_UINT64(event->id, event->parent_id);
            ++hashes;
        }
        if (strcmp(event->name, "model.cache.entity") == 0) {
            TEST_ASSERT_EQUAL_UINT64(71u, event->value_u64);
            ++entities;
        }
    }
    TEST_ASSERT_EQUAL_UINT(1, failures);
    TEST_ASSERT_EQUAL_UINT(1, hashes);
    TEST_ASSERT_EQUAL_UINT(1, entities);
    TEST_ASSERT_EQUAL_UINT(1, warnings);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
}

static void diagnostics_reset_rearms_each_owner_without_a_registry(void)
{
    other_owner = JCE_CALLOC(1, sizeof(*other_owner));
    TEST_ASSERT_NOT_NULL(other_owner);
    fixture_table_create(other_owner);
    JceSceneRenderer *other = other_owner;
    sr_model_cache_failure(owner, "missing.glb", 9, SR_MODEL_FAILURE_NO_ARCHIVE);
    sr_model_cache_failure(other, "missing.glb", 9, SR_MODEL_FAILURE_NO_ARCHIVE);
    TEST_ASSERT_EQUAL_UINT(2, warnings);
    owner->model_lookup_hint = fixture_entry(owner, 0);
    owner->model_upload_frame_valid = true;
    sr_model_cache_diagnostics_reset(owner);
    TEST_ASSERT_EQUAL_UINT64(1, owner->model_cache_generation);
    TEST_ASSERT_EQUAL_UINT64(0, other->model_cache_generation);
    TEST_ASSERT_NULL(owner->model_lookup_hint);
    TEST_ASSERT_FALSE(owner->model_upload_frame_valid);
    sr_model_cache_failure(owner, "missing.glb", 10, SR_MODEL_FAILURE_NO_ARCHIVE);
    TEST_ASSERT_EQUAL_UINT(3, warnings);
    TEST_ASSERT_NOT_NULL(strstr(last_warning, "generation=1"));
    fixture_renderer_destroy(&other_owner);
}

static void disabled_trace_keeps_one_warning_and_no_work_items(void)
{
    jce_trace_set_enabled(false);
    for (unsigned i = 0; i < 20u; ++i)
        sr_model_cache_failure(owner, "missing.glb", i, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_failure(NULL, "missing.glb", 0, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_failure(owner, "missing.glb", 0, (JceSrModelFailure)99);
    TEST_ASSERT_EQUAL_UINT(1, warnings);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
}

static JceTraceEvent observed[2048];
static uint32_t observed_count;

static void read_observed(void)
{
    JceTraceCursor cursor;
    jce_trace_cursor_init(&cursor);
    observed_count = jce_trace_read(&cursor, observed, 2048);
    TEST_ASSERT_LESS_THAN_UINT32(2048, observed_count);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
}

static uint64_t detailed_packet(void)
{
    for (uint32_t i = 0; i < observed_count; ++i)
        if (observed[i].type == JCE_TRACE_EVENT_TASK_BEGIN &&
            strcmp(observed[i].name, "sr.model.cache.reject") == 0)
            return observed[i].id;
    TEST_FAIL_MESSAGE("Missing model rejection detail");
    return 0;
}

static uint64_t packet(unsigned ordinal)
{
    for (uint32_t i = 0; i < observed_count; ++i)
        if (observed[i].type == JCE_TRACE_EVENT_TASK_BEGIN &&
            strcmp(observed[i].name, "sr.model.cache.reject_summary") == 0 &&
            ordinal-- == 0) return observed[i].id;
    TEST_FAIL_MESSAGE("Missing model rejection summary");
    return 0;
}

static uint64_t counter(uint64_t id, const char *name)
{
    for (uint32_t i = 0; i < observed_count; ++i)
        if (observed[i].type == JCE_TRACE_EVENT_COUNTER && observed[i].id == id &&
            strcmp(observed[i].name, name) == 0) {
            TEST_ASSERT_EQUAL_UINT64(id, observed[i].parent_id);
            return observed[i].value_u64;
        }
    TEST_FAIL_MESSAGE(name);
    return 0;
}

static uint64_t wide_counter(uint64_t id, const char *name)
{
    char key[64];
    snprintf(key, sizeof(key), "%s_lo", name);
    const uint64_t lo = counter(id, key);
    snprintf(key, sizeof(key), "%s_hi", name);
    return lo | (counter(id, key) << 32u);
}

static unsigned summary_count(void)
{
    unsigned count = 0;
    for (uint32_t i = 0; i < observed_count; ++i)
        count += observed[i].type == JCE_TRACE_EVENT_TASK_BEGIN &&
            strcmp(observed[i].name, "sr.model.cache.reject_summary") == 0;
    return count;
}

static void unallocated_table_reports_zero_capacity_without_reading_storage(void)
{
    fixture_table_destroy(owner);
    sr_model_cache_failure_hash(owner, 0x11, 10, SR_MODEL_FAILURE_ENTRY_BUDGET);
    TEST_ASSERT_EQUAL_UINT64(0, occupancy_reads);
    TEST_ASSERT_NOT_NULL(strstr(last_warning, "occupied=0 limit=0"));
    read_observed();
    const uint64_t detail = detailed_packet();
    TEST_ASSERT_EQUAL_UINT64(2, counter(detail, "model.cache.schema"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(detail, "model.cache.occupied"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(detail, "model.cache.limit"));
    TEST_ASSERT_EQUAL_UINT64(SR_MODEL_CACHE_MAX, counter(detail, "model.cache.max_limit"));
}

static void sparse_table_counts_all_states_and_separates_capacity_from_maximum(void)
{
    fixture_entry(owner, 0)->model = (JceModel *)owner;
    fixture_entry(owner, 63)->pending = true;
    fixture_entry(owner, 128)->failed = true;
    (void)fixture_entry(owner, MODEL_DIAGNOSTICS_FIXTURE_CAPACITY - 1u);
    sr_model_cache_failure_hash(owner, 0x22, 20, SR_MODEL_FAILURE_ENTRY_BUDGET);
    TEST_ASSERT_EQUAL_UINT64(MODEL_DIAGNOSTICS_FIXTURE_CAPACITY, occupancy_reads);
    TEST_ASSERT_NOT_NULL(strstr(last_warning, "occupied=4 limit=256"));
    TEST_ASSERT_NOT_NULL(strstr(last_warning, "loaded=1 pending=1 failed=1 unavailable=1"));
    read_observed();
    const uint64_t detail = detailed_packet();
    TEST_ASSERT_EQUAL_UINT64(4, counter(detail, "model.cache.occupied"));
    TEST_ASSERT_EQUAL_UINT64(MODEL_DIAGNOSTICS_FIXTURE_CAPACITY,
        counter(detail, "model.cache.limit"));
    TEST_ASSERT_EQUAL_UINT64(SR_MODEL_CACHE_MAX, counter(detail, "model.cache.max_limit"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(detail, "model.cache.loaded"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(detail, "model.cache.pending"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(detail, "model.cache.failed"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(detail, "model.cache.unavailable"));
}

static void exact_repeats_flush_once_per_round_without_rewalking_slots(void)
{
    const void *scene = &last_warning;
    sr_model_cache_diagnostics_bind(owner, scene, 7, 0);
    for (unsigned i = 0; i < 4096u; ++i)
        sr_model_cache_failure_hash(owner, 0x1234u + i, 71u + i, SR_MODEL_FAILURE_ENTRY_BUDGET);
    sr_model_cache_diagnostics_bind(owner, scene, 7, 0);
    TEST_ASSERT_EQUAL_UINT64(4096, owner->model_diagnostics.reasons[1].pending);
    sr_model_cache_diagnostics_bind(owner, scene, 7, 1);
    sr_model_cache_failure_hash(owner, 0xabcdu, 9000, SR_MODEL_FAILURE_ENTRY_BUDGET);
    sr_model_cache_diagnostics_bind(owner, scene, 7, 2);
    TEST_ASSERT_TRUE(sr_model_cache_diagnostics_flush(owner, SR_MODEL_DIAGNOSTICS_DESTROY));
    read_observed();
    TEST_ASSERT_EQUAL_UINT(2, summary_count());
    const uint64_t first = packet(0), second = packet(1);
    TEST_ASSERT_EQUAL_UINT64(4096, wide_counter(first, "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(4096, wide_counter(first, "model.cache.total"));
    TEST_ASSERT_EQUAL_UINT64(1, wide_counter(second, "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(4097, wide_counter(second, "model.cache.total"));
    TEST_ASSERT_EQUAL_UINT64(71, counter(first, "model.cache.first_entity"));
    TEST_ASSERT_EQUAL_UINT64(4166, counter(first, "model.cache.last_entity"));
    TEST_ASSERT_EQUAL_UINT64(0x1234, counter(first, "model.cache.first_path_hash"));
    TEST_ASSERT_EQUAL_UINT64(0x2233, counter(first, "model.cache.last_path_hash"));
    TEST_ASSERT_EQUAL_UINT64(9000, counter(second, "model.cache.first_entity"));
    TEST_ASSERT_EQUAL_UINT64((uintptr_t)scene, wide_counter(first, "model.cache.scene"));
    TEST_ASSERT_EQUAL_UINT64(7, wide_counter(first, "model.cache.context"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(first, "model.cache.schema"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(first, "model.cache.context_available"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(first, "model.cache.backend_cpu_frame"));
    TEST_ASSERT_EQUAL_UINT64(SR_MODEL_DIAGNOSTICS_ROUND, counter(first, "model.cache.flush_reason"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(first, "model.cache.per_request_details"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(first, "model.cache.count_complete"));
    TEST_ASSERT_EQUAL_UINT64(MODEL_DIAGNOSTICS_FIXTURE_CAPACITY, occupancy_reads);
}

static void each_reason_and_owner_keeps_an_independent_exact_batch(void)
{
    sr_model_cache_failure_hash(owner, 0x11, 10, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_failure_hash(owner, 0x22, 20, SR_MODEL_FAILURE_ENTRY_BUDGET);
    sr_model_cache_failure_hash(owner, 0x33, 30, SR_MODEL_FAILURE_LOAD);
    TEST_ASSERT_TRUE(sr_model_cache_diagnostics_flush(owner, SR_MODEL_DIAGNOSTICS_DESTROY));
    read_observed();
    TEST_ASSERT_EQUAL_UINT(2, summary_count());
    TEST_ASSERT_EQUAL_UINT64(SR_MODEL_FAILURE_LOAD, counter(packet(0), "model.cache.reason"));
    TEST_ASSERT_EQUAL_UINT64(2, wide_counter(packet(0), "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(10, counter(packet(0), "model.cache.first_entity"));
    TEST_ASSERT_EQUAL_UINT64(30, counter(packet(0), "model.cache.last_entity"));
    TEST_ASSERT_EQUAL_UINT64(SR_MODEL_FAILURE_ENTRY_BUDGET, counter(packet(1), "model.cache.reason"));
    TEST_ASSERT_EQUAL_UINT64(1, wide_counter(packet(1), "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(packet(0), "model.cache.context_available"));
    TEST_ASSERT_EQUAL_UINT64(MODEL_DIAGNOSTICS_FIXTURE_CAPACITY * 2u, occupancy_reads);
}

static void scene_and_context_changes_close_the_old_identity_before_rearming(void)
{
    const void *a = &owner, *b = &last_warning;
    sr_model_cache_diagnostics_bind(owner, a, 7, 0);
    sr_model_cache_failure_hash(owner, 11, 10, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_diagnostics_bind(owner, b, 7, 0);
    sr_model_cache_failure_hash(owner, 22, 20, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_diagnostics_bind(owner, b, 8, 0);
    sr_model_cache_failure_hash(owner, 33, 30, SR_MODEL_FAILURE_LOAD);
    TEST_ASSERT_TRUE(sr_model_cache_diagnostics_flush(owner, SR_MODEL_DIAGNOSTICS_DESTROY));
    read_observed();
    TEST_ASSERT_EQUAL_UINT(3, summary_count());
    TEST_ASSERT_EQUAL_UINT64((uintptr_t)a, wide_counter(packet(0), "model.cache.scene"));
    TEST_ASSERT_EQUAL_UINT64((uintptr_t)b, wide_counter(packet(1), "model.cache.scene"));
    TEST_ASSERT_EQUAL_UINT64(7, wide_counter(packet(1), "model.cache.context"));
    TEST_ASSERT_EQUAL_UINT64(8, wide_counter(packet(2), "model.cache.context"));
    TEST_ASSERT_EQUAL_UINT64(1, wide_counter(packet(2), "model.cache.total"));
    TEST_ASSERT_EQUAL_UINT64(SR_MODEL_DIAGNOSTICS_IDENTITY, counter(packet(0), "model.cache.flush_reason"));
    TEST_ASSERT_EQUAL_UINT64(MODEL_DIAGNOSTICS_FIXTURE_CAPACITY * 3u, occupancy_reads);
    TEST_ASSERT_EQUAL_UINT(1, warnings);
}

static void reset_flushes_before_its_generation_changes_and_destroy_flushes_unbound(void)
{
    sr_model_cache_failure_hash(owner, 11, 10, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_failure_hash(owner, 22, 20, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_diagnostics_reset(owner);
    sr_model_cache_failure_hash(owner, 33, 30, SR_MODEL_FAILURE_LOAD);
    jce_model_diagnostics_test_destroy_prefix(owner);
    read_observed();
    TEST_ASSERT_EQUAL_UINT(2, summary_count());
    TEST_ASSERT_EQUAL_UINT64(2, wide_counter(packet(0), "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(packet(0), "model.cache.generation"));
    TEST_ASSERT_EQUAL_UINT64(SR_MODEL_DIAGNOSTICS_RESET, counter(packet(0), "model.cache.flush_reason"));
    TEST_ASSERT_EQUAL_UINT64(1, wide_counter(packet(1), "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(packet(1), "model.cache.generation"));
    TEST_ASSERT_EQUAL_UINT64(SR_MODEL_DIAGNOSTICS_DESTROY, counter(packet(1), "model.cache.flush_reason"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(packet(1), "model.cache.context_available"));
    TEST_ASSERT_EQUAL_UINT64(0, owner->model_diagnostics.reasons[0].pending);
}

static void preexisting_warning_does_not_consume_the_first_trace_witness(void)
{
    jce_trace_set_enabled(false);
    sr_model_cache_failure_hash(owner, 11, 10, SR_MODEL_FAILURE_LOAD);
    TEST_ASSERT_EQUAL_UINT(1, warnings);
    jce_trace_set_enabled(true);
    sr_model_cache_failure_hash(owner, 22, 20, SR_MODEL_FAILURE_LOAD);
    TEST_ASSERT_TRUE(sr_model_cache_diagnostics_flush(owner, SR_MODEL_DIAGNOSTICS_DESTROY));
    read_observed();
    TEST_ASSERT_EQUAL_UINT(1, summary_count());
    TEST_ASSERT_EQUAL_UINT64(1, wide_counter(packet(0), "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(20, counter(packet(0), "model.cache.first_entity"));
    unsigned details = 0;
    for (uint32_t i = 0; i < observed_count; ++i)
        details += observed[i].type == JCE_TRACE_EVENT_TASK_BEGIN &&
            strcmp(observed[i].name, "sr.model.cache.reject") == 0;
    TEST_ASSERT_EQUAL_UINT(1, details);
    TEST_ASSERT_EQUAL_UINT(1, warnings);
}

static void capture_restart_preserves_old_pending_identity_without_claiming_new_coverage(void)
{
    sr_model_cache_diagnostics_bind(owner, &owner, 7, 0);
    sr_model_cache_failure_hash(owner, 11, 10, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_failure_hash(owner, 22, 20, SR_MODEL_FAILURE_LOAD);
    const uint32_t old_capture = owner->model_diagnostics.batch.capture_generation;
    jce_trace_set_enabled(false);
    TEST_ASSERT_FALSE(sr_model_cache_diagnostics_flush(owner, SR_MODEL_DIAGNOSTICS_DESTROY));
    sr_model_cache_diagnostics_reset(owner);
    sr_model_cache_diagnostics_bind(owner, &last_warning, 8, 0);
    jce_trace_set_enabled(true);
    jce_trace_reset();
    sr_model_cache_failure_hash(owner, 33, 30, SR_MODEL_FAILURE_LOAD);
    TEST_ASSERT_TRUE(sr_model_cache_diagnostics_flush(owner, SR_MODEL_DIAGNOSTICS_DESTROY));
    read_observed();
    TEST_ASSERT_EQUAL_UINT(2, summary_count());
    TEST_ASSERT_EQUAL_UINT64(old_capture, counter(packet(0), "model.cache.capture_generation"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(packet(0), "model.cache.current_capture"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(packet(1), "model.cache.current_capture"));
    TEST_ASSERT_NOT_EQUAL_UINT64(old_capture, counter(packet(0), "model.cache.published_capture"));
    TEST_ASSERT_EQUAL_UINT64(SR_MODEL_DIAGNOSTICS_CAPTURE, counter(packet(0), "model.cache.flush_reason"));
    TEST_ASSERT_EQUAL_UINT64(2, wide_counter(packet(0), "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(packet(0), "model.cache.generation"));
    TEST_ASSERT_EQUAL_UINT64(7, wide_counter(packet(0), "model.cache.context"));
    TEST_ASSERT_EQUAL_UINT64(1, wide_counter(packet(1), "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(packet(1), "model.cache.generation"));
    TEST_ASSERT_EQUAL_UINT64(8, wide_counter(packet(1), "model.cache.context"));
    TEST_ASSERT_EQUAL_UINT64(counter(packet(1), "model.cache.capture_generation"),
        counter(packet(1), "model.cache.published_capture"));
}

static void backend_frame_wrap_and_zero_are_exact_equality_boundaries(void)
{
    sr_model_cache_diagnostics_bind(owner, &owner, 1, UINT32_MAX);
    sr_model_cache_failure_hash(owner, 11, 10, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_diagnostics_bind(owner, &owner, 1, 0);
    sr_model_cache_failure_hash(owner, 22, 20, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_diagnostics_bind(owner, &owner, 1, 0);
    TEST_ASSERT_TRUE(sr_model_cache_diagnostics_flush(owner, SR_MODEL_DIAGNOSTICS_DESTROY));
    read_observed();
    TEST_ASSERT_EQUAL_UINT(2, summary_count());
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, counter(packet(0), "model.cache.backend_cpu_frame"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(packet(1), "model.cache.backend_cpu_frame"));
    TEST_ASSERT_EQUAL_UINT64(2, wide_counter(packet(1), "model.cache.total"));
    TEST_ASSERT_EQUAL_UINT64(MODEL_DIAGNOSTICS_FIXTURE_CAPACITY, occupancy_reads);
}

static void saturated_counts_report_a_lower_bound_instead_of_wrapping(void)
{
    sr_model_cache_failure_hash(owner, 11, 10, SR_MODEL_FAILURE_LOAD);
    owner->model_diagnostics.reasons[0].pending = UINT64_MAX;
    owner->model_diagnostics.reasons[0].total = UINT64_MAX;
    sr_model_cache_failure_hash(owner, 22, 20, SR_MODEL_FAILURE_LOAD);
    TEST_ASSERT_TRUE(sr_model_cache_diagnostics_flush(owner, SR_MODEL_DIAGNOSTICS_DESTROY));
    read_observed();
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, wide_counter(packet(0), "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, wide_counter(packet(0), "model.cache.total"));
    TEST_ASSERT_EQUAL_UINT64(1, counter(packet(0), "model.cache.count_saturated"));
    TEST_ASSERT_EQUAL_UINT64(0, counter(packet(0), "model.cache.count_complete"));
    TEST_ASSERT_EQUAL_UINT64(20, counter(packet(0), "model.cache.last_entity"));
}

static void separate_renderers_never_combine_request_counts(void)
{
    other_owner = JCE_CALLOC(1, sizeof(*other_owner));
    TEST_ASSERT_NOT_NULL(other_owner);
    fixture_table_create(other_owner);
    JceSceneRenderer *other = other_owner;
    sr_model_cache_failure_hash(owner, 11, 10, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_failure_hash(owner, 22, 20, SR_MODEL_FAILURE_LOAD);
    sr_model_cache_failure_hash(other, 33, 30, SR_MODEL_FAILURE_LOAD);
    const bool first_closed = sr_model_cache_diagnostics_flush(owner, SR_MODEL_DIAGNOSTICS_DESTROY);
    const bool second_closed = sr_model_cache_diagnostics_flush(other, SR_MODEL_DIAGNOSTICS_DESTROY);
    const uintptr_t other_identity = (uintptr_t)other;
    fixture_renderer_destroy(&other_owner);
    TEST_ASSERT_TRUE(first_closed);
    TEST_ASSERT_TRUE(second_closed);
    read_observed();
    TEST_ASSERT_EQUAL_UINT(2, summary_count());
    TEST_ASSERT_EQUAL_UINT64((uintptr_t)owner, counter(packet(0), "model.cache.owner"));
    TEST_ASSERT_EQUAL_UINT64(other_identity, counter(packet(1), "model.cache.owner"));
    TEST_ASSERT_EQUAL_UINT64(2, wide_counter(packet(0), "model.cache.requests"));
    TEST_ASSERT_EQUAL_UINT64(1, wide_counter(packet(1), "model.cache.requests"));
}

int main(void)
{
    printf("model_diagnostics_bytes=%zu reason_slots=%u\n",
        sizeof(SrModelDiagnostics), (unsigned)SR_MODEL_FAILURE_REASONS);
    UNITY_BEGIN();
    RUN_TEST(all_occupied_states_count_and_warning_is_bounded);
    RUN_TEST(repeated_rejection_is_bounded_without_repeated_occupancy_walks);
    RUN_TEST(diagnostics_reset_rearms_each_owner_without_a_registry);
    RUN_TEST(disabled_trace_keeps_one_warning_and_no_work_items);
    RUN_TEST(unallocated_table_reports_zero_capacity_without_reading_storage);
    RUN_TEST(sparse_table_counts_all_states_and_separates_capacity_from_maximum);
    RUN_TEST(exact_repeats_flush_once_per_round_without_rewalking_slots);
    RUN_TEST(each_reason_and_owner_keeps_an_independent_exact_batch);
    RUN_TEST(scene_and_context_changes_close_the_old_identity_before_rearming);
    RUN_TEST(reset_flushes_before_its_generation_changes_and_destroy_flushes_unbound);
    RUN_TEST(preexisting_warning_does_not_consume_the_first_trace_witness);
    RUN_TEST(capture_restart_preserves_old_pending_identity_without_claiming_new_coverage);
    RUN_TEST(backend_frame_wrap_and_zero_are_exact_equality_boundaries);
    RUN_TEST(saturated_counts_report_a_lower_bound_instead_of_wrapping);
    RUN_TEST(separate_renderers_never_combine_request_counts);
    return UNITY_END();
}
