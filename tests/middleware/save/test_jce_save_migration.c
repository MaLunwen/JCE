/*
 * test_jce_save_migration.c — Unit tests for the save-migration registry
 * (jce_save_migration.h, L4 save).
 *
 * Exercises the REAL snapshot save/load path: a test section is written at an
 * old version into a real JSNP buffer, then loaded back through the real
 * jce_snapshot_load_from_buffer().  The reader's read_fn parses the section's
 * JSON and calls the real jce_save_migrate() to walk a v1->v2->v3 chain
 * before consuming it.  No mocks — the migration runs inside the live load.
 *
 * Migration story for the "stats" section:
 *   v1 JSON: { "hp": <int> }
 *   v1->v2 : rename "hp" -> "health"   (field rename)
 *   v2->v3 : scale  "health" *= 10      (value transform)
 *   v3 JSON: { "health": <int*10> }
 */

#include "unity.h"

#include <jce/middleware/save/jce_snapshot.h>
#include <jce/middleware/save/jce_save_migration.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_alloc.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define STATS_ID    "stats"

/* Migration step call counters (proves no-op skips, and chain ordering). */
static int g_v1_to_v2_calls = 0;
static int g_v2_to_v3_calls = 0;

static void reset_counters(void)
{
    g_v1_to_v2_calls = 0;
    g_v2_to_v3_calls = 0;
}

/* ------------------------------------------------------------------ */
/* Migration steps (transform parsed JSON in place)                     */
/* ------------------------------------------------------------------ */

/* v1 -> v2 : rename "hp" to "health". */
static bool mig_hp_to_health(JceJson *json, void *user)
{
    (void)user;
    if (!jce_json_has(json, "hp")) return false;     /* malformed → abort */
    int hp = jce_json_get_int(json, "hp", 0);
    jce_json_remove(json, "hp");
    jce_json_set_int(json, "health", hp);
    g_v1_to_v2_calls++;
    return true;
}

/* v2 -> v3 : scale "health" by 10. */
static bool mig_scale_health(JceJson *json, void *user)
{
    (void)user;
    if (!jce_json_has(json, "health")) return false;
    int h = jce_json_get_int(json, "health", 0);
    jce_json_remove(json, "health");
    jce_json_set_int(json, "health", h * 10);
    g_v2_to_v3_calls++;
    return true;
}

/* ------------------------------------------------------------------ */
/* Writer-side provider: serializes a stats JSON document.              */
/* The section version is whatever the registration's current_version is.*/
/* ------------------------------------------------------------------ */

typedef struct {
    const char *json_text;  /* document to write verbatim */
} StatsWriter;

static bool stats_write(JceSnapshotStream *s, void *user)
{
    const StatsWriter *w = (const StatsWriter *)user;
    return jce_snap_write_string(s, w->json_text);
}

/* ------------------------------------------------------------------ */
/* Reader-side provider: parses payload JSON, MIGRATES via the real     */
/* registry from loaded_version up to current, then captures the result.*/
/* ------------------------------------------------------------------ */

#define STATS_CURRENT_VERSION 3u

typedef struct {
    JceSaveMigrationRegistry *migrations;  /* may be NULL */
    /* Outputs captured after migration: */
    bool     applied;       /* read_fn ran load to completion */
    bool     has_health;    /* "health" key present post-migration */
    bool     has_hp;        /* "hp" key still present (should be false at v3) */
    int      health;        /* migrated value */
} StatsReader;

static bool stats_read(JceSnapshotStream *s, uint32_t loaded_version, void *user)
{
    StatsReader *rd = (StatsReader *)user;

    char *text = NULL;
    if (!jce_snap_read_string(s, &text) || !text) return false;
    JceJson *root = jce_json_parse(text, 0);
    jce_free(text);
    if (!root) return false;

    /* Real migration walk: no-op when loaded_version == current. */
    if (!jce_save_migrate(rd->migrations, STATS_ID,
                          loaded_version, STATS_CURRENT_VERSION, root)) {
        jce_json_free(root);
        return false;   /* propagate failure into snapshot load */
    }

    rd->has_health = jce_json_has(root, "health");
    rd->has_hp     = jce_json_has(root, "hp");
    rd->health     = jce_json_get_int(root, "health", -1);
    rd->applied    = true;
    jce_json_free(root);
    return true;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Write a single "stats" section at `version` carrying `json_text`. */
static bool make_blob(uint32_t version, const char *json_text,
                      void **out_buf, size_t *out_size)
{
    StatsWriter w = { json_text };
    JceSnapshotRegistry *reg = jce_snapshot_registry_create();
    jce_snapshot_register(reg, STATS_ID, version, stats_write, NULL, &w);
    bool ok = jce_snapshot_save_to_buffer(reg, out_buf, out_size);
    jce_snapshot_registry_destroy(reg);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Registry unit tests (direct jce_save_migrate, no snapshot)           */
/* ------------------------------------------------------------------ */

static void test_register_rejects_non_advancing(void)
{
    JceSaveMigrationRegistry *m = jce_save_migration_registry_create();
    TEST_ASSERT_NOT_NULL(m);
    /* to <= from must be rejected. */
    TEST_ASSERT_FALSE(jce_save_migration_register(m, STATS_ID, 2, 2,
                                                  mig_hp_to_health, NULL));
    TEST_ASSERT_FALSE(jce_save_migration_register(m, STATS_ID, 3, 1,
                                                  mig_hp_to_health, NULL));
    TEST_ASSERT_TRUE(jce_save_migration_register(m, STATS_ID, 1, 2,
                                                 mig_hp_to_health, NULL));
    /* Duplicate (id, from) rejected; first stays. */
    TEST_ASSERT_FALSE(jce_save_migration_register(m, STATS_ID, 1, 2,
                                                  mig_hp_to_health, NULL));
    jce_save_migration_registry_destroy(m);
}

static void test_migrate_current_version_is_noop(void)
{
    reset_counters();
    JceSaveMigrationRegistry *m = jce_save_migration_registry_create();
    jce_save_migration_register(m, STATS_ID, 1, 2, mig_hp_to_health, NULL);
    jce_save_migration_register(m, STATS_ID, 2, 3, mig_scale_health, NULL);

    JceJson *j = jce_json_object();
    jce_json_set_int(j, "health", 50);
    /* from == to → strict no-op, no step called, JSON untouched. */
    TEST_ASSERT_TRUE(jce_save_migrate(m, STATS_ID, 3, 3, j));
    TEST_ASSERT_EQUAL_INT(0, g_v1_to_v2_calls);
    TEST_ASSERT_EQUAL_INT(0, g_v2_to_v3_calls);
    TEST_ASSERT_EQUAL_INT(50, jce_json_get_int(j, "health", -1));

    jce_json_free(j);
    jce_save_migration_registry_destroy(m);
}

static void test_migrate_missing_step_fails(void)
{
    reset_counters();
    JceSaveMigrationRegistry *m = jce_save_migration_registry_create();
    /* Only v2->v3 registered; v1->v2 is MISSING. */
    jce_save_migration_register(m, STATS_ID, 2, 3, mig_scale_health, NULL);

    JceJson *j = jce_json_object();
    jce_json_set_int(j, "hp", 5);
    /* v1 -> v3 cannot complete: missing v1 step must be reported as failure. */
    TEST_ASSERT_FALSE(jce_save_migrate(m, STATS_ID, 1, 3, j));
    TEST_ASSERT_EQUAL_INT(0, g_v2_to_v3_calls);  /* never reached */

    jce_json_free(j);
    jce_save_migration_registry_destroy(m);
}

static void test_migrate_downgrade_fails(void)
{
    JceSaveMigrationRegistry *m = jce_save_migration_registry_create();
    JceJson *j = jce_json_object();
    /* from > to (a future save) → refuse. */
    TEST_ASSERT_FALSE(jce_save_migrate(m, STATS_ID, 3, 1, j));
    jce_json_free(j);
    jce_save_migration_registry_destroy(m);
}

/* ------------------------------------------------------------------ */
/* REAL snapshot load + migration                                       */
/* ------------------------------------------------------------------ */

/* Write a v1 blob, load through the real snapshot with a full chain,
 * assert data arrives migrated to v3 (hp:5 -> health:50). */
static void test_real_load_migrates_v1_to_v3(void)
{
    reset_counters();
    void *buf = NULL; size_t size = 0;
    TEST_ASSERT_TRUE(make_blob(1u, "{\"hp\":5}", &buf, &size));

    JceSaveMigrationRegistry *m = jce_save_migration_registry_create();
    jce_save_migration_register(m, STATS_ID, 1, 2, mig_hp_to_health, NULL);
    jce_save_migration_register(m, STATS_ID, 2, 3, mig_scale_health, NULL);

    StatsReader rd = {0};
    rd.migrations = m;
    JceSnapshotRegistry *reg = jce_snapshot_registry_create();
    jce_snapshot_register(reg, STATS_ID, STATS_CURRENT_VERSION,
                          NULL, stats_read, &rd);

    /* The REAL load drives the section read_fn at loaded_version=1. */
    TEST_ASSERT_TRUE(jce_snapshot_load_from_buffer(reg, buf, size));

    TEST_ASSERT_TRUE(rd.applied);
    TEST_ASSERT_TRUE(rd.has_health);
    TEST_ASSERT_FALSE(rd.has_hp);          /* renamed away */
    TEST_ASSERT_EQUAL_INT(50, rd.health);  /* 5 -> 5(health) -> 50 */
    TEST_ASSERT_EQUAL_INT(1, g_v1_to_v2_calls);
    TEST_ASSERT_EQUAL_INT(1, g_v2_to_v3_calls);

    jce_free(buf);
    jce_snapshot_registry_destroy(reg);
    jce_save_migration_registry_destroy(m);
}

/* A current-version (v3) blob loads with ZERO migration-step calls. */
static void test_real_load_current_version_skips_migration(void)
{
    reset_counters();
    void *buf = NULL; size_t size = 0;
    TEST_ASSERT_TRUE(make_blob(STATS_CURRENT_VERSION,
                               "{\"health\":77}", &buf, &size));

    JceSaveMigrationRegistry *m = jce_save_migration_registry_create();
    jce_save_migration_register(m, STATS_ID, 1, 2, mig_hp_to_health, NULL);
    jce_save_migration_register(m, STATS_ID, 2, 3, mig_scale_health, NULL);

    StatsReader rd = {0};
    rd.migrations = m;
    JceSnapshotRegistry *reg = jce_snapshot_registry_create();
    jce_snapshot_register(reg, STATS_ID, STATS_CURRENT_VERSION,
                          NULL, stats_read, &rd);

    TEST_ASSERT_TRUE(jce_snapshot_load_from_buffer(reg, buf, size));

    TEST_ASSERT_TRUE(rd.applied);
    TEST_ASSERT_TRUE(rd.has_health);
    TEST_ASSERT_EQUAL_INT(77, rd.health);          /* untouched */
    TEST_ASSERT_EQUAL_INT(0, g_v1_to_v2_calls);     /* NO migration */
    TEST_ASSERT_EQUAL_INT(0, g_v2_to_v3_calls);

    jce_free(buf);
    jce_snapshot_registry_destroy(reg);
    jce_save_migration_registry_destroy(m);
}

/* A v1 blob with NO migration path through the real load fails the load
 * (the read_fn returns false), rather than silently delivering raw v1
 * data — corruption is rejected, not consumed. */
static void test_real_load_missing_step_fails_load(void)
{
    reset_counters();
    void *buf = NULL; size_t size = 0;
    TEST_ASSERT_TRUE(make_blob(1u, "{\"hp\":5}", &buf, &size));

    /* Only v2->v3 registered; v1->v2 missing → migrate fails → load fails. */
    JceSaveMigrationRegistry *m = jce_save_migration_registry_create();
    jce_save_migration_register(m, STATS_ID, 2, 3, mig_scale_health, NULL);

    StatsReader rd = {0};
    rd.migrations = m;
    JceSnapshotRegistry *reg = jce_snapshot_registry_create();
    jce_snapshot_register(reg, STATS_ID, STATS_CURRENT_VERSION,
                          NULL, stats_read, &rd);

    TEST_ASSERT_FALSE(jce_snapshot_load_from_buffer(reg, buf, size));
    TEST_ASSERT_FALSE(rd.applied);   /* never reached the apply point */

    jce_free(buf);
    jce_snapshot_registry_destroy(reg);
    jce_save_migration_registry_destroy(m);
}

/* With NO migration registry at all (NULL), an old save also fails the load
 * (matches the scene provider's NULL-migrations behavior). */
static void test_real_load_null_registry_fails_old_save(void)
{
    reset_counters();
    void *buf = NULL; size_t size = 0;
    TEST_ASSERT_TRUE(make_blob(1u, "{\"hp\":5}", &buf, &size));

    StatsReader rd = {0};
    rd.migrations = NULL;   /* no registry wired */
    JceSnapshotRegistry *reg = jce_snapshot_registry_create();
    jce_snapshot_register(reg, STATS_ID, STATS_CURRENT_VERSION,
                          NULL, stats_read, &rd);

    TEST_ASSERT_FALSE(jce_snapshot_load_from_buffer(reg, buf, size));
    TEST_ASSERT_FALSE(rd.applied);

    jce_free(buf);
    jce_snapshot_registry_destroy(reg);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_register_rejects_non_advancing);
    RUN_TEST(test_migrate_current_version_is_noop);
    RUN_TEST(test_migrate_missing_step_fails);
    RUN_TEST(test_migrate_downgrade_fails);
    RUN_TEST(test_real_load_migrates_v1_to_v3);
    RUN_TEST(test_real_load_current_version_skips_migration);
    RUN_TEST(test_real_load_missing_step_fails_load);
    RUN_TEST(test_real_load_null_registry_fails_old_save);
    return UNITY_END();
}
