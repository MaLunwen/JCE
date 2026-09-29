/* test_jce_input_serialize.c
 *
 * Headless JSON file round-trip for the editor-authored input_actions.json
 * (Input Manager panel <-> jce_actions_save_file / jce_actions_load_file).
 *
 * The pure in-memory schemes/composite tests live in test_jce_input_schemes.c
 * and test_jce_input_composite.c; THIS file proves the on-disk schema survives
 * a full save->reload, which is exactly what the Input Manager panel relies on
 * to persist authored:
 *   - per-binding device_group tags,
 *   - composite sub-keys (1D axis pos/neg + 2D vector up/down/left/right) and
 *     the JceCompositeKind carried in JceBinding.code,
 *   - the schemes[] block (name + device_mask) and the active_scheme selection.
 *
 * Each test builds a table, saves it to a temp file, reloads it, and asserts
 * the reloaded table is field-identical — mirroring the panel's
 * build_engine_table() -> jce_actions_save_file() -> jce_actions_load_file()
 * -> copy_from_engine() round-trip.
 */

#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_keys.h>
#include <jce/os/platform/jce_gamepad.h>

#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TMP_PATH  "test_jce_input_serialize.tmp.json"
/* A second name so the space-map round-trip never shares a file with the
 * five save_reload() tests -- several sessions run this suite against the
 * same build directory at once, and ctest gives every test the same CWD. */
#define TMP_PATH2 "test_jce_input_serialize.space.tmp.json"

void setUp(void) {}
void tearDown(void) { remove(TMP_PATH); remove(TMP_PATH2); }

/* Save `a` to TMP_PATH and reload it; caller owns the returned table. */
static JceInputActions *save_reload(const JceInputActions *a)
{
    TEST_ASSERT_TRUE(jce_actions_save_file(a, TMP_PATH));
    JceInputActions *r = jce_actions_load_file(TMP_PATH);
    TEST_ASSERT_NOT_NULL(r);
    return r;
}

/* A per-binding device_group tag round-trips through the file. */
static void test_device_group_roundtrips(void)
{
    JceInputActions *a = jce_actions_create();
    int fire = jce_action_register(a, "fire");
    TEST_ASSERT_TRUE(fire >= 0);

    JceBinding kb;
    jce_binding_init(&kb, JCE_SRC_KEY, JCE_KEY_SPACE);
    kb.device_group = JCE_DEVICE_KBM;
    TEST_ASSERT_TRUE(jce_action_bind(a, fire, &kb));

    JceBinding gb;
    jce_binding_init(&gb, JCE_SRC_PAD_BUTTON, JCE_GAMEPAD_BUTTON_SOUTH);
    gb.device_group = JCE_DEVICE_GAMEPAD;
    TEST_ASSERT_TRUE(jce_action_bind(a, fire, &gb));

    JceInputActions *r = save_reload(a);

    int rfire = jce_action_find(r, "fire");
    TEST_ASSERT_TRUE(rfire >= 0);
    TEST_ASSERT_EQUAL_INT(2, jce_action_bind_count(r, rfire));

    JceBinding b0, b1;
    TEST_ASSERT_TRUE(jce_action_bind_at(r, rfire, 0, &b0));
    TEST_ASSERT_TRUE(jce_action_bind_at(r, rfire, 1, &b1));
    TEST_ASSERT_EQUAL_INT(JCE_DEVICE_KBM,     b0.device_group);
    TEST_ASSERT_EQUAL_INT(JCE_DEVICE_GAMEPAD, b1.device_group);
    /* The SOURCE has to survive too, and it goes to disk as a schema-1 int
     * (0 KEY / 2 GAMEPAD_BTN) and comes back through bind_type_from_v1.  Break
     * either half of that translation and these two go red rather than the
     * gates in space. */
    TEST_ASSERT_EQUAL_INT(JCE_SRC_KEY,        b0.type);
    TEST_ASSERT_EQUAL_INT(JCE_SRC_PAD_BUTTON, b1.type);

    jce_actions_destroy(r);
    jce_actions_destroy(a);
}

/* A device-agnostic (JCE_DEVICE_NONE) binding round-trips as 0 (the saver
 * omits the key, the loader defaults it to 0). */
static void test_device_none_roundtrips_as_zero(void)
{
    JceInputActions *a = jce_actions_create();
    int pause = jce_action_register(a, "pause");
    JceBinding b;
    jce_binding_init(&b, JCE_SRC_KEY, JCE_KEY_ESCAPE);
    /* device_group left at JCE_DEVICE_NONE. */
    TEST_ASSERT_TRUE(jce_action_bind(a, pause, &b));

    JceInputActions *r = save_reload(a);
    int rp = jce_action_find(r, "pause");
    JceBinding rb;
    TEST_ASSERT_TRUE(jce_action_bind_at(r, rp, 0, &rb));
    TEST_ASSERT_EQUAL_INT(JCE_DEVICE_NONE, rb.device_group);

    jce_actions_destroy(r);
    jce_actions_destroy(a);
}

/* Composite 2D-vector + 1D-axis sub-keys and the JceCompositeKind (code)
 * survive the file round-trip. */
static void test_composite_roundtrips(void)
{
    JceInputActions *a = jce_actions_create();

    int move = jce_action_register(a, "move");
    JceBinding v;
    jce_binding_init(&v, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    v.comp[JCE_COMP_POS].type  = (int16_t)JCE_SRC_KEY;
    v.comp[JCE_COMP_POS].code  = JCE_KEY_D;  /* +x right */
    v.comp[JCE_COMP_NEG].type  = (int16_t)JCE_SRC_KEY;
    v.comp[JCE_COMP_NEG].code  = JCE_KEY_A;  /* -x left  */
    v.comp[JCE_COMP_UP].type   = (int16_t)JCE_SRC_KEY;
    v.comp[JCE_COMP_UP].code   = JCE_KEY_W;  /* +y up    */
    v.comp[JCE_COMP_DOWN].type = (int16_t)JCE_SRC_KEY;
    v.comp[JCE_COMP_DOWN].code = JCE_KEY_S;  /* -y down  */
    v.device_group = JCE_DEVICE_KBM;
    TEST_ASSERT_TRUE(jce_action_bind(a, move, &v));

    int turn = jce_action_register(a, "turn");
    JceBinding x;
    jce_binding_init(&x, JCE_SRC_COMPOSITE, JCE_COMPOSITE_AXIS_1D);
    x.comp[JCE_COMP_POS].type = (int16_t)JCE_SRC_KEY;
    x.comp[JCE_COMP_POS].code = JCE_KEY_E;   /* + */
    x.comp[JCE_COMP_NEG].type = (int16_t)JCE_SRC_KEY;
    x.comp[JCE_COMP_NEG].code = JCE_KEY_Q;   /* - */
    TEST_ASSERT_TRUE(jce_action_bind(a, turn, &x));

    JceInputActions *r = save_reload(a);

    JceBinding rv;
    TEST_ASSERT_TRUE(jce_action_bind_at(r, jce_action_find(r, "move"), 0, &rv));
    TEST_ASSERT_EQUAL_INT(JCE_SRC_COMPOSITE,       rv.type);
    TEST_ASSERT_EQUAL_INT(JCE_COMPOSITE_VECTOR_2D, rv.code);
    TEST_ASSERT_EQUAL_INT(JCE_SRC_KEY, rv.comp[JCE_COMP_POS].type);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_D,   rv.comp[JCE_COMP_POS].code);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_A,   rv.comp[JCE_COMP_NEG].code);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_W,   rv.comp[JCE_COMP_UP].code);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_S,   rv.comp[JCE_COMP_DOWN].code);
    TEST_ASSERT_EQUAL_INT(JCE_DEVICE_KBM, rv.device_group);

    JceBinding rx;
    TEST_ASSERT_TRUE(jce_action_bind_at(r, jce_action_find(r, "turn"), 0, &rx));
    TEST_ASSERT_EQUAL_INT(JCE_COMPOSITE_AXIS_1D, rx.code);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_E, rx.comp[JCE_COMP_POS].code);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_Q, rx.comp[JCE_COMP_NEG].code);
    /* 1D axis: up/down unused, and "unused" is now a TYPE, not a zero code. */
    TEST_ASSERT_EQUAL_INT(JCE_SRC_NONE, rx.comp[JCE_COMP_UP].type);
    TEST_ASSERT_EQUAL_INT(JCE_SRC_NONE, rx.comp[JCE_COMP_DOWN].type);

    jce_actions_destroy(r);
    jce_actions_destroy(a);
}

/* Named control schemes (name + device_mask) and an explicit active-scheme
 * selection survive the file round-trip. */
static void test_schemes_roundtrip(void)
{
    JceInputActions *a = jce_actions_create();
    (void)jce_action_register(a, "fire");   /* loader needs >=1 action */

    int kbm = jce_action_scheme_register(a, "KeyboardMouse",
                                         JCE_DEVICE_BIT(JCE_DEVICE_KBM));
    int pad = jce_action_scheme_register(a, "Gamepad",
                                         JCE_DEVICE_BIT(JCE_DEVICE_GAMEPAD));
    TEST_ASSERT_TRUE(kbm >= 0 && pad >= 0);

    /* Pin Gamepad as the active scheme (non-default first-scheme selection). */
    TEST_ASSERT_TRUE(jce_action_scheme_set_active(a, pad));
    TEST_ASSERT_EQUAL_INT(pad, jce_action_scheme_active(a));

    JceInputActions *r = save_reload(a);

    TEST_ASSERT_EQUAL_INT(2, jce_action_scheme_count(r));
    int rkbm = jce_action_scheme_find(r, "KeyboardMouse");
    int rpad = jce_action_scheme_find(r, "Gamepad");
    TEST_ASSERT_TRUE(rkbm >= 0 && rpad >= 0);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVICE_BIT(JCE_DEVICE_KBM),
                          (int)jce_action_scheme_mask(r, rkbm));
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVICE_BIT(JCE_DEVICE_GAMEPAD),
                          (int)jce_action_scheme_mask(r, rpad));
    /* The explicitly-selected active scheme survived. */
    TEST_ASSERT_EQUAL_INT(rpad, jce_action_scheme_active(r));

    jce_actions_destroy(r);
    jce_actions_destroy(a);
}

/* A scheme-free map stays scheme-free after the round-trip (the saver omits
 * the schemes block, the loader keeps scheme_count==0 / active==-1). */
static void test_no_schemes_roundtrip_stays_legacy(void)
{
    JceInputActions *a = jce_actions_create();
    int fire = jce_action_register(a, "fire");
    JceBinding b;
    jce_binding_init(&b, JCE_SRC_KEY, JCE_KEY_SPACE);
    TEST_ASSERT_TRUE(jce_action_bind(a, fire, &b));

    JceInputActions *r = save_reload(a);
    TEST_ASSERT_EQUAL_INT(0, jce_action_scheme_count(r));
    TEST_ASSERT_EQUAL_INT(-1, jce_action_scheme_active(r));

    jce_actions_destroy(r);
    jce_actions_destroy(a);
}

static void test_memory_loader_matches_file_schema(void)
{
    static const char source[] =
        "{\"actions\":["
        "{\"name\":\"space_begin\",\"binds\":["
        "{\"type\":0,\"code\":44,\"scale\":1,\"deadzone\":0}]},"
        "{\"name\":\"space_pause\",\"binds\":["
        "{\"type\":0,\"code\":19,\"scale\":1,\"deadzone\":0}]}]}";
    JceInputActions *a = jce_actions_load_memory(source, sizeof(source) - 1u);
    TEST_ASSERT_NOT_NULL(a);

    int begin = jce_action_find(a, "space_begin");
    int pause = jce_action_find(a, "space_pause");
    TEST_ASSERT_TRUE(begin >= 0);
    TEST_ASSERT_TRUE(pause >= 0);

    JceBinding binding;
    TEST_ASSERT_TRUE(jce_action_bind_at(a, begin, 0, &binding));
    TEST_ASSERT_EQUAL_INT(JCE_BIND_KEY, binding.type);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_SPACE, binding.code);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, binding.scale);
    TEST_ASSERT_TRUE(jce_action_bind_at(a, pause, 0, &binding));
    TEST_ASSERT_EQUAL_INT(JCE_KEY_P, binding.code);
    jce_actions_destroy(a);
}

/* ── the real shipped map, not a literal that agrees with the code ────── */

/* Collect every "type": N integer in a JSON file, in order.  Textual on
 * purpose: it reads the DISK numbering without going through the loader that
 * is under test, so the two cannot agree with each other by construction. */
static int collect_disk_types(const char *path, int *out, int max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    int count = 0;
    for (const char *p = buf; (p = strstr(p, "\"type\"")) != NULL; ) {
        p += 6;
        while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (count >= max) return -2;
        out[count++] = atoi(p);
    }
    return count;
}

/* JceBinding v2 renumbers the source enum, and schema 1 froze the old numbers
 * on disk.  This is the file that pays for a mistake there: every bind in the
 * tracked space map is "type": 0, so a missing bind_type_from_v1 turns all of
 * them into JCE_SRC_NONE and every space action goes dead -- which the space
 * determinism gates would report as a determinism failure, with nothing
 * pointing at input.
 *
 * Deliberately loads the REAL tracked file rather than a string literal: a
 * literal in this file would only prove the writer and the reader agree with
 * each other.  Counts are not hard-coded either, so the space team can edit
 * their own map without breaking an input test. */
static void test_the_shipped_space_map_survives_the_v1_round_trip(void)
{
    int disk_before[256];
    int nd = collect_disk_types(JCE_SPACE_INPUT_MAP_PATH, disk_before, 256);
    TEST_ASSERT_TRUE_MESSAGE(nd > 0, JCE_SPACE_INPUT_MAP_PATH);

    JceInputActions *a = jce_actions_load_file(JCE_SPACE_INPUT_MAP_PATH);
    TEST_ASSERT_NOT_NULL_MESSAGE(a, JCE_SPACE_INPUT_MAP_PATH);

    int binds = 0;
    for (int i = 0; i < jce_actions_count(a); ++i) {
        for (int b = 0; b < jce_action_bind_count(a, i); ++b) {
            JceBinding bd;
            TEST_ASSERT_TRUE(jce_action_bind_at(a, i, b, &bd));
            TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(
                JCE_SRC_NONE, bd.type,
                "a shipped binding loaded as JCE_SRC_NONE -- the schema-1 "
                "source translation is gone and this action is now dead");
            ++binds;
        }
    }
    TEST_ASSERT_EQUAL_INT(nd, binds);   /* every disk bind reached memory */

    /* And back out again: the ints written must be the ints that were read,
     * or the next load of the file the writer just produced means something
     * different from the file it replaced. */
    TEST_ASSERT_TRUE(jce_actions_save_file(a, TMP_PATH2));
    int disk_after[256];
    int na = collect_disk_types(TMP_PATH2, disk_after, 256);
    TEST_ASSERT_EQUAL_INT(nd, na);
    for (int i = 0; i < nd; ++i)
        TEST_ASSERT_EQUAL_INT(disk_before[i], disk_after[i]);

    jce_actions_destroy(a);
}

/* ── the schema-1 source translation, all five cases ──────────────────── */

/* bind_type_to_v1() and bind_type_from_v1() are two hand-mirrored switches in
 * jce_input_actions.c with nothing pinning them as inverses, and before this
 * test only three of their five cases were reachable from any test in this
 * directory: KEY (0) and PAD_BUTTON (2) through test_device_group_roundtrips,
 * COMPOSITE (4) through test_composite_roundtrips.  MOUSE_BUTTON (1) and
 * PAD_AXIS (3) were never written and re-read anywhere -- both tracked space
 * maps are 100 % "type": 0 -- so `case 3: return JCE_SRC_PAD_STICK;`, which
 * kills every stick binding in any v1 map that has one, was green across the
 * whole suite.
 *
 * The second half re-saves the reloaded table and compares the two files byte
 * for byte, which pins the OTHER direction: to_v1(from_v1(v)) == v for every
 * int the writer emitted. */
static void test_every_schema1_source_survives_the_round_trip(void)
{
    static const struct { const char *name; int type; int code; } k_all[] = {
        { "s_key",      JCE_SRC_KEY,          JCE_KEY_SPACE            },
        { "s_mousebtn", JCE_SRC_MOUSE_BUTTON, 3                        },
        { "s_padbtn",   JCE_SRC_PAD_BUTTON,   JCE_GAMEPAD_BUTTON_SOUTH },
        { "s_padaxis",  JCE_SRC_PAD_AXIS,     JCE_GAMEPAD_AXIS_LEFTX   },
        { "s_comp",     JCE_SRC_COMPOSITE,    JCE_COMPOSITE_AXIS_1D    },
    };
    const int n = (int)(sizeof k_all / sizeof k_all[0]);

    JceInputActions *a = jce_actions_create();
    TEST_ASSERT_NOT_NULL(a);
    for (int i = 0; i < n; ++i) {
        int id = jce_action_register(a, k_all[i].name);
        TEST_ASSERT_TRUE(id >= 0);
        JceBinding b;
        jce_binding_init(&b, (JceBindType)k_all[i].type, k_all[i].code);
        if (k_all[i].type == JCE_SRC_COMPOSITE) {
            b.comp[JCE_COMP_POS].type = (int16_t)JCE_SRC_KEY;
            b.comp[JCE_COMP_POS].code = JCE_KEY_E;
            b.comp[JCE_COMP_NEG].type = (int16_t)JCE_SRC_KEY;
            b.comp[JCE_COMP_NEG].code = JCE_KEY_Q;
        }
        TEST_ASSERT_TRUE(jce_action_bind(a, id, &b));
    }

    JceInputActions *r = save_reload(a);
    for (int i = 0; i < n; ++i) {
        int id = jce_action_find(r, k_all[i].name);
        TEST_ASSERT_TRUE_MESSAGE(id >= 0, k_all[i].name);
        JceBinding out;
        TEST_ASSERT_TRUE(jce_action_bind_at(r, id, 0, &out));
        TEST_ASSERT_EQUAL_INT_MESSAGE(k_all[i].type, out.type, k_all[i].name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(k_all[i].code, out.code, k_all[i].name);
    }

    /* from_v1 . to_v1 == identity is pinned above; to_v1 . from_v1 == identity
     * is pinned here, by re-emitting what was just read. */
    int first[64];
    int nf = collect_disk_types(TMP_PATH, first, 64);
    TEST_ASSERT_EQUAL_INT(n, nf);
    TEST_ASSERT_TRUE(jce_actions_save_file(r, TMP_PATH2));
    int second[64];
    int ns = collect_disk_types(TMP_PATH2, second, 64);
    TEST_ASSERT_EQUAL_INT(nf, ns);
    for (int i = 0; i < nf; ++i)
        TEST_ASSERT_EQUAL_INT_MESSAGE(first[i], second[i],
            "the schema-1 int a reload re-emits differs from the one it read");

    jce_actions_destroy(r);
    jce_actions_destroy(a);
}

/* Schema 1 has ONE bare int per composite sub-slot and no room for a type, so
 * the loader re-types every non-zero code as JCE_SRC_KEY.  The writer must
 * therefore refuse to emit the code of a sub-source that is not a key: doing
 * so re-creates the exact defect this record was introduced to kill -- a
 * D-pad sub-source coming back as the letter with the same number.
 *
 * DPAD_LEFT is 13 and JCE_KEY_J is 13; DPAD_RIGHT is 14 and JCE_KEY_K is 14. */
static void test_typed_pad_sub_source_is_not_written_as_a_key(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_KEY_J, JCE_GAMEPAD_BUTTON_DPAD_LEFT);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_K, JCE_GAMEPAD_BUTTON_DPAD_RIGHT);

    JceInputActions *a = jce_actions_create();
    int id = jce_action_register(a, "nav");
    TEST_ASSERT_TRUE(id >= 0);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    b.comp[JCE_COMP_POS].type  = (int16_t)JCE_SRC_PAD_BUTTON;
    b.comp[JCE_COMP_POS].code  = JCE_GAMEPAD_BUTTON_DPAD_RIGHT;
    b.comp[JCE_COMP_NEG].type  = (int16_t)JCE_SRC_PAD_BUTTON;
    b.comp[JCE_COMP_NEG].code  = JCE_GAMEPAD_BUTTON_DPAD_LEFT;
    /* One real key so the composite still has something to carry across. */
    b.comp[JCE_COMP_UP].type   = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_UP].code   = JCE_KEY_W;
    /* A slot that is absent but still holds a stale code: it must not
     * resurrect as a live key either. */
    b.comp[JCE_COMP_DOWN].type = (int16_t)JCE_SRC_NONE;
    b.comp[JCE_COMP_DOWN].code = JCE_KEY_S;
    TEST_ASSERT_TRUE(jce_action_bind(a, id, &b));

    JceInputActions *r = save_reload(a);
    int rid = jce_action_find(r, "nav");
    TEST_ASSERT_TRUE(rid >= 0);
    JceBinding out;
    TEST_ASSERT_TRUE(jce_action_bind_at(r, rid, 0, &out));

    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_SRC_NONE, out.comp[JCE_COMP_POS].type,
        "a JCE_SRC_PAD_BUTTON sub-source reloaded as a live source");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, out.comp[JCE_COMP_POS].code,
        "DPAD_RIGHT (14) was written to disk and came back as key K (14)");
    TEST_ASSERT_EQUAL_INT(JCE_SRC_NONE, out.comp[JCE_COMP_NEG].type);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, out.comp[JCE_COMP_NEG].code,
        "DPAD_LEFT (13) was written to disk and came back as key J (13)");

    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_SRC_NONE, out.comp[JCE_COMP_DOWN].type,
        "an absent slot carrying a stale code resurrected on reload");
    TEST_ASSERT_EQUAL_INT(0, out.comp[JCE_COMP_DOWN].code);

    /* The key half is untouched: the guard drops what schema 1 cannot say,
     * not the whole binding. */
    TEST_ASSERT_EQUAL_INT(JCE_SRC_KEY, out.comp[JCE_COMP_UP].type);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_W,   out.comp[JCE_COMP_UP].code);

    jce_actions_destroy(r);
    jce_actions_destroy(a);
}

static void test_memory_loader_rejects_truncated_json(void)
{
    static const char source[] = "{\"actions\":[";
    TEST_ASSERT_NULL(jce_actions_load_memory(source, sizeof(source) - 1u));
    TEST_ASSERT_NULL(jce_actions_load_memory(NULL, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_device_group_roundtrips);
    RUN_TEST(test_device_none_roundtrips_as_zero);
    RUN_TEST(test_composite_roundtrips);
    RUN_TEST(test_schemes_roundtrip);
    RUN_TEST(test_no_schemes_roundtrip_stays_legacy);
    RUN_TEST(test_memory_loader_matches_file_schema);
    RUN_TEST(test_the_shipped_space_map_survives_the_v1_round_trip);
    RUN_TEST(test_every_schema1_source_survives_the_round_trip);
    RUN_TEST(test_typed_pad_sub_source_is_not_written_as_a_key);
    RUN_TEST(test_memory_loader_rejects_truncated_json);
    return UNITY_END();
}
