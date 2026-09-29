/*
 * test_jce_snapshot.c — Unit tests for jce_snapshot.h (L3 save).
 *
 * Buffer-only round-trip (no file I/O).  Verifies provider registry,
 * stream primitives, header peek, version negotiation, and that
 * unknown sections are silently skipped on load.
 */

#include "unity.h"

#include <jce/middleware/save/jce_snapshot.h>
#include <jce/os/core/jce_alloc.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Test providers                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t u;
    float    f;
    char     name[16];
} Player;

static bool write_player(JceSnapshotStream *s, void *user)
{
    const Player *p = (const Player *)user;
    if (!jce_snap_write_u32(s, p->u))    return false;
    if (!jce_snap_write_f32(s, p->f))    return false;
    if (!jce_snap_write_string(s, p->name)) return false;
    return true;
}

static bool read_player(JceSnapshotStream *s, uint32_t version, void *user)
{
    Player *p = (Player *)user;
    if (version != 1) return false;
    char *name = NULL;
    if (!jce_snap_read_u32(s, &p->u)) return false;
    if (!jce_snap_read_f32(s, &p->f)) return false;
    if (!jce_snap_read_string(s, &name)) return false;
    strncpy(p->name, name, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';
    jce_free(name);
    return true;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

static void test_create_destroy(void)
{
    JceSnapshotRegistry *r = jce_snapshot_registry_create();
    TEST_ASSERT_NOT_NULL(r);
    jce_snapshot_registry_destroy(r);
    jce_snapshot_registry_destroy(NULL);  /* must not crash */
    TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* Round-trip                                                           */
/* ------------------------------------------------------------------ */

static void test_round_trip_preserves_data(void)
{
    Player in  = { 42u, 3.5f, "Alice" };
    Player out = { 0, 0, {0} };

    JceSnapshotRegistry *w = jce_snapshot_registry_create();
    jce_snapshot_register(w, "player", 1, write_player, read_player, &in);
    void  *buf  = NULL;
    size_t size = 0;
    TEST_ASSERT_TRUE(jce_snapshot_save_to_buffer(w, &buf, &size));
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(size > 0);

    JceSnapshotRegistry *r = jce_snapshot_registry_create();
    jce_snapshot_register(r, "player", 1, write_player, read_player, &out);
    TEST_ASSERT_TRUE(jce_snapshot_load_from_buffer(r, buf, size));

    TEST_ASSERT_EQUAL_UINT32(in.u, out.u);
    TEST_ASSERT_EQUAL_FLOAT(in.f, out.f);
    TEST_ASSERT_EQUAL_STRING(in.name, out.name);

    jce_free(buf);
    jce_snapshot_registry_destroy(w);
    jce_snapshot_registry_destroy(r);
}

/* ------------------------------------------------------------------ */
/* Header peek                                                          */
/* ------------------------------------------------------------------ */

static void test_peek_header_returns_section_count(void)
{
    Player p = { 1, 1.0f, "X" };
    JceSnapshotRegistry *w = jce_snapshot_registry_create();
    jce_snapshot_register(w, "player", 1, write_player, read_player, &p);
    jce_snapshot_register(w, "world",  1, write_player, read_player, &p);

    void *buf = NULL; size_t size = 0;
    TEST_ASSERT_TRUE(jce_snapshot_save_to_buffer(w, &buf, &size));

    JceSnapshotHeaderInfo hdr = {0};
    TEST_ASSERT_TRUE(jce_snapshot_peek_header(buf, size, &hdr));
    TEST_ASSERT_EQUAL_UINT32(1u, hdr.format_version);
    TEST_ASSERT_EQUAL_UINT32(2u, hdr.section_count);

    jce_free(buf);
    jce_snapshot_registry_destroy(w);
}

static void test_peek_header_rejects_garbage(void)
{
    const uint8_t junk[8] = { 'X','X','X','X', 0,0,0,0 };
    JceSnapshotHeaderInfo hdr = {0};
    TEST_ASSERT_FALSE(jce_snapshot_peek_header(junk, sizeof(junk), &hdr));
}

/* ------------------------------------------------------------------ */
/* Unknown sections are skipped                                         */
/* ------------------------------------------------------------------ */

static void test_load_skips_unknown_sections(void)
{
    Player in  = { 7, 9.0f, "Bob" };
    JceSnapshotRegistry *w = jce_snapshot_registry_create();
    jce_snapshot_register(w, "ghost", 1, write_player, read_player, &in);
    jce_snapshot_register(w, "player", 1, write_player, read_player, &in);
    void *buf = NULL; size_t size = 0;
    TEST_ASSERT_TRUE(jce_snapshot_save_to_buffer(w, &buf, &size));

    /* Reader only registers "player" — "ghost" should be silently skipped. */
    Player out = { 0, 0, {0} };
    JceSnapshotRegistry *r = jce_snapshot_registry_create();
    jce_snapshot_register(r, "player", 1, write_player, read_player, &out);
    TEST_ASSERT_TRUE(jce_snapshot_load_from_buffer(r, buf, size));
    TEST_ASSERT_EQUAL_UINT32(in.u, out.u);
    TEST_ASSERT_EQUAL_STRING(in.name, out.name);

    jce_free(buf);
    jce_snapshot_registry_destroy(w);
    jce_snapshot_registry_destroy(r);
}

/* ------------------------------------------------------------------ */
/* Unregister                                                           */
/* ------------------------------------------------------------------ */

static void test_unregister_removes_provider(void)
{
    Player p = { 1, 1.0f, "X" };
    JceSnapshotRegistry *r = jce_snapshot_registry_create();
    jce_snapshot_register(r, "player", 1, write_player, read_player, &p);
    jce_snapshot_unregister(r, "player");

    void *buf = NULL; size_t size = 0;
    TEST_ASSERT_TRUE(jce_snapshot_save_to_buffer(r, &buf, &size));
    JceSnapshotHeaderInfo hdr = {0};
    TEST_ASSERT_TRUE(jce_snapshot_peek_header(buf, size, &hdr));
    TEST_ASSERT_EQUAL_UINT32(0u, hdr.section_count);

    jce_free(buf);
    jce_snapshot_registry_destroy(r);
}

/* ------------------------------------------------------------------ */
/* Load rejects corrupt data                                            */
/* ------------------------------------------------------------------ */

static void test_load_rejects_garbage(void)
{
    JceSnapshotRegistry *r = jce_snapshot_registry_create();
    const uint8_t junk[16] = {0};
    TEST_ASSERT_FALSE(jce_snapshot_load_from_buffer(r, junk, sizeof(junk)));
    jce_snapshot_registry_destroy(r);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_round_trip_preserves_data);
    RUN_TEST(test_peek_header_returns_section_count);
    RUN_TEST(test_peek_header_rejects_garbage);
    RUN_TEST(test_load_skips_unknown_sections);
    RUN_TEST(test_unregister_removes_provider);
    RUN_TEST(test_load_rejects_garbage);
    return UNITY_END();
}
