/*
 * test_jce_json.c — Unit tests for jce_json.h
 *
 * Layer: L1.  Thin facade over cJSON (no direct cjson dep).
 */

#include "unity.h"

#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_filesystem.h>

#include <stdio.h>
#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

static void test_parse_object_and_typed_get(void)
{
    const char *src = "{\"n\":42,\"b\":true,\"s\":\"hi\"}";
    JceJson *j = jce_json_parse(src, 0);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_TRUE(jce_json_is_object(j));

    TEST_ASSERT_EQUAL_INT(42, jce_json_get_int(j, "n", -1));
    TEST_ASSERT_TRUE(jce_json_get_bool(j, "b", false));
    TEST_ASSERT_EQUAL_DOUBLE(42.0, jce_json_get_number(j, "n", -1.0));
    TEST_ASSERT_EQUAL_INT(99, jce_json_get_int(j, "missing", 99));

    TEST_ASSERT_TRUE(jce_json_has(j, "n"));
    TEST_ASSERT_FALSE(jce_json_has(j, "missing"));

    jce_json_free(j);
}

static void test_parse_array_navigation(void)
{
    JceJson *j = jce_json_parse("[10,20,30]", 0);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_TRUE(jce_json_is_array(j));
    TEST_ASSERT_EQUAL_INT(3, jce_json_array_size(j));

    JceJson *e1 = jce_json_array_at(j, 1);
    TEST_ASSERT_NOT_NULL(e1);
    TEST_ASSERT_EQUAL_DOUBLE(20.0, jce_json_number_value(e1, -1.0));

    jce_json_free(j);
}

static void test_parse_invalid_returns_null(void)
{
    TEST_ASSERT_NULL(jce_json_parse("{not json", 0));
}

static void test_build_and_print_roundtrip(void)
{
    JceJson *o = jce_json_object();
    TEST_ASSERT_NOT_NULL(o);
    jce_json_set_int   (o, "n", 5);
    jce_json_set_string(o, "s", "hi");
    jce_json_set_bool  (o, "b", true);

    char *out = jce_json_print(o, false);
    TEST_ASSERT_NOT_NULL(out);

    JceJson *r = jce_json_parse(out, 0);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT(5, jce_json_get_int(r, "n", -1));
    TEST_ASSERT_TRUE(jce_json_get_bool(r, "b", false));

    jce_json_free_string(out);
    jce_json_free(r);
    jce_json_free(o);
}

static void test_get_xyz_with_default(void)
{
    JceJson *o = jce_json_parse("{\"posX\":1.0,\"posY\":2.0,\"posZ\":3.0}", 0);
    TEST_ASSERT_NOT_NULL(o);
    float v[3] = { 0 };
    jce_json_get_xyz(o, "pos", v, NULL);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v[0]);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, v[1]);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, v[2]);
    jce_json_free(o);
}

/* ---- additional API surface (was uncovered before) ------------------ */

static void test_get_xyzw_falls_back_to_default(void)
{
    JceJson *o = jce_json_parse("{\"vX\":7.0}", 0);
    TEST_ASSERT_NOT_NULL(o);
    float def[4] = { -1, -2, -3, -4 };
    float v[4];
    jce_json_get_xyzw(o, "v", v, def);
    TEST_ASSERT_EQUAL_FLOAT( 7.0f, v[0]);
    TEST_ASSERT_EQUAL_FLOAT(-2.0f, v[1]);
    TEST_ASSERT_EQUAL_FLOAT(-3.0f, v[2]);
    TEST_ASSERT_EQUAL_FLOAT(-4.0f, v[3]);
    jce_json_free(o);
}

static void test_get_floats_with_default(void)
{
    JceJson *o = jce_json_parse("{\"a\":[1.0,2.0,3.0]}", 0);
    float def[3] = { 9, 9, 9 };
    float out[3];
    jce_json_get_floats(o, "a", out, 3, def);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, out[0]);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, out[1]);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, out[2]);

    /* Missing key: all defaults. */
    jce_json_get_floats(o, "missing", out, 3, def);
    TEST_ASSERT_EQUAL_FLOAT(9.0f, out[0]);
    TEST_ASSERT_EQUAL_FLOAT(9.0f, out[2]);

    /* Array shorter than n: all defaults. */
    float out4[4] = { 0 };
    float def4[4] = { 5, 5, 5, 5 };
    jce_json_get_floats(o, "a", out4, 4, def4);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, out4[0]);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, out4[3]);

    jce_json_free(o);
}

static void test_get_string_and_string_value(void)
{
    JceJson *o = jce_json_parse("{\"s\":\"hello\"}", 0);
    TEST_ASSERT_EQUAL_STRING("hello", jce_json_get_string(o, "s",       "x"));
    TEST_ASSERT_EQUAL_STRING("dflt",  jce_json_get_string(o, "missing", "dflt"));

    JceJson *node = jce_json_get(o, "s");
    TEST_ASSERT_TRUE(jce_json_is_string(node));
    TEST_ASSERT_EQUAL_STRING("hello", jce_json_string_value(node, "x"));
    TEST_ASSERT_EQUAL_STRING("x",     jce_json_string_value(NULL, "x"));

    jce_json_free(o);
}

static void test_any_of_keys(void)
{
    JceJson *o = jce_json_parse("{\"alpha\":1.5,\"name\":\"foo\"}", 0);
    const char *num_keys[] = { "missing", "alpha", "other" };
    TEST_ASSERT_EQUAL_DOUBLE(1.5, jce_json_get_number_any(o, num_keys, 3, -1.0));
    TEST_ASSERT_EQUAL_DOUBLE(-1.0,
        jce_json_get_number_any(o, (const char *const []){ "a", "b" }, 2, -1.0));

    const char *str_keys[] = { "label", "name" };
    TEST_ASSERT_EQUAL_STRING("foo", jce_json_get_string_any(o, str_keys, 2, "def"));
    TEST_ASSERT_EQUAL_STRING("def",
        jce_json_get_string_any(o, (const char *const []){ "x" }, 1, "def"));

    jce_json_free(o);
}

static void test_object_iteration_and_member_key(void)
{
    JceJson *o = jce_json_parse("{\"a\":1,\"b\":2,\"c\":3}", 0);
    int sum = 0; int count = 0;
    for (JceJson *it = jce_json_first_child(o); it; it = jce_json_next_sibling(it)) {
        const char *k = jce_json_member_key(it);
        TEST_ASSERT_NOT_NULL(k);
        TEST_ASSERT_EQUAL_INT(1, (int)strlen(k));
        sum += (int)jce_json_number_value(it, 0.0);
        count++;
    }
    TEST_ASSERT_EQUAL_INT(3, count);
    TEST_ASSERT_EQUAL_INT(6, sum);
    jce_json_free(o);
}

static void test_builders_object_array_and_attach(void)
{
    JceJson *root = jce_json_object();
    jce_json_set_number(root, "f", 3.14);
    jce_json_set_int   (root, "i", 7);
    jce_json_set_bool  (root, "b", false);
    jce_json_set_string(root, "s", "x");

    JceJson *arr = jce_json_array();
    jce_json_array_push_number(arr, 1.0);
    jce_json_array_push_number(arr, 2.0);
    jce_json_array_push_string(arr, "three");
    jce_json_array_push(arr, jce_json_bool(true));
    TEST_ASSERT_EQUAL_INT(4, jce_json_array_size(arr));
    jce_json_set_child(root, "arr", arr);

    JceJson *child = jce_json_object();
    jce_json_set_number(child, "k", 9.0);
    jce_json_set_child(root, "child", child);

    float xyz[3]  = { 1.0f, 2.0f, 3.0f };
    float xyzw[4] = { 4.0f, 5.0f, 6.0f, 7.0f };
    float fa[2]   = { 11.0f, 22.0f };
    jce_json_set_xyz       (root, "pos",  xyz);
    jce_json_set_xyzw      (root, "rot",  xyzw);
    jce_json_set_float_array(root, "fa",  fa, 2);

    char *pretty  = jce_json_print(root, true);
    char *compact = jce_json_print(root, false);
    TEST_ASSERT_NOT_NULL(pretty);
    TEST_ASSERT_NOT_NULL(compact);
    TEST_ASSERT_GREATER_THAN_INT((int)strlen(compact), (int)strlen(pretty));

    JceJson *re = jce_json_parse(compact, 0);
    TEST_ASSERT_EQUAL_INT(7, jce_json_get_int(re, "i", 0));
    TEST_ASSERT_EQUAL_FLOAT(2.0f,
        (float)jce_json_number_value(jce_json_array_at(jce_json_get(re, "arr"), 1), 0.0));

    jce_json_free_string(pretty);
    jce_json_free_string(compact);
    jce_json_free(re);
    jce_json_free(root);
}

static void test_parse_with_explicit_length(void)
{
    const char *src = "{\"n\":1}GARBAGE";
    JceJson *j = jce_json_parse(src, 7);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(1, jce_json_get_int(j, "n", -1));
    jce_json_free(j);
}

static void test_parse_file_and_write_file_roundtrip(void)
{
    char base[1024];
    (void)jce_fs_host_get_current_dir(base, sizeof(base));
    char path[1024];
    snprintf(path, sizeof(path), "%s/_ut_json.json", base);
    (void)jce_fs_host_remove_file(path);

    JceJson *o = jce_json_object();
    jce_json_set_int(o, "x", 42);
    TEST_ASSERT_TRUE(jce_json_write_file(path, o, true, true));
    /* `o` was freed because take_ownership=true. */

    JceJson *r = jce_json_parse_file(path);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT(42, jce_json_get_int(r, "x", 0));
    jce_json_free(r);

    TEST_ASSERT_NULL(jce_json_parse_file("does/not/exist/_x.json"));

    (void)jce_fs_host_remove_file(path);
}

static void test_parse_file_respects_isolated_vfs_miss(void)
{
    const char *path = "_ut_json_isolated_host_only.json";
    (void)jce_fs_host_remove_file(path);

    JceJson *host = jce_json_object();
    jce_json_set_bool(host, "hostOnly", true);
    TEST_ASSERT_TRUE(jce_json_write_file(path, host, true, true));

    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_ISOLATED);

    TEST_ASSERT_NULL(jce_json_parse_file(path));

    jce_fs_set_active(NULL);
    jce_fs_destroy(fs);
    (void)jce_fs_host_remove_file(path);
}

static void test_type_predicates_and_constructors(void)
{
    JceJson *n = jce_json_number(1.5);
    JceJson *s = jce_json_string("z");
    JceJson *b = jce_json_bool(true);
    JceJson *a = jce_json_array();
    JceJson *o = jce_json_object();
    TEST_ASSERT_TRUE (jce_json_is_number(n));
    TEST_ASSERT_TRUE (jce_json_is_string(s));
    TEST_ASSERT_TRUE (jce_json_is_bool  (b));
    TEST_ASSERT_TRUE (jce_json_is_array (a));
    TEST_ASSERT_TRUE (jce_json_is_object(o));
    TEST_ASSERT_FALSE(jce_json_is_object(a));
    jce_json_free(n); jce_json_free(s); jce_json_free(b);
    jce_json_free(a); jce_json_free(o);
}

/* ------------------------------------------------------------------ *
 *  jce_json_detach — the MOVE primitive.
 *
 *  Distinct from jce_json_remove, which destroys the subtree.  Detach
 *  unlinks and hands ownership to the caller so a node can be relocated
 *  into another document without a print/re-parse round trip (the world
 *  partitioner moves entity nodes into per-cell fragments this way).
 *  Getting this wrong is not a visible-behaviour bug — it is a leak or a
 *  double free — so the re-attach and free paths are both exercised here,
 *  under the leak checker the suite already runs.
 * ------------------------------------------------------------------ */

static void test_detach_moves_node_between_documents(void)
{
    JceJson *src = jce_json_parse("{\"a\":[{\"id\":1},{\"id\":2},{\"id\":3}]}", 0);
    TEST_ASSERT_NOT_NULL(src);
    JceJson *arr = jce_json_get(src, "a");
    TEST_ASSERT_EQUAL_INT(3, jce_json_array_size(arr));

    JceJson *mid = jce_json_array_at(arr, 1);
    TEST_ASSERT_NOT_NULL(mid);
    TEST_ASSERT_EQUAL_INT(2, jce_json_get_int(mid, "id", -1));

    jce_json_detach(arr, mid);
    /* Unlinked from the source... */
    TEST_ASSERT_EQUAL_INT(2, jce_json_array_size(arr));
    TEST_ASSERT_EQUAL_INT(1, jce_json_get_int(jce_json_array_at(arr, 0), "id", -1));
    TEST_ASSERT_EQUAL_INT(3, jce_json_get_int(jce_json_array_at(arr, 1), "id", -1));
    /* ...but NOT freed: still readable, and re-attachable elsewhere. */
    TEST_ASSERT_EQUAL_INT(2, jce_json_get_int(mid, "id", -1));

    JceJson *dst = jce_json_array();
    TEST_ASSERT_NOT_NULL(dst);
    jce_json_array_push(dst, mid);          /* dst now owns mid */
    TEST_ASSERT_EQUAL_INT(1, jce_json_array_size(dst));
    TEST_ASSERT_EQUAL_INT(2, jce_json_get_int(jce_json_array_at(dst, 0), "id", -1));

    jce_json_free(dst);                     /* frees mid exactly once */
    jce_json_free(src);
}

/* A detached node the caller never re-attaches must be freeable on its own —
   this is the path that leaks if detach is mistaken for remove. */
static void test_detach_then_free_standalone(void)
{
    JceJson *root = jce_json_parse("{\"a\":[{\"k\":\"v\"}]}", 0);
    TEST_ASSERT_NOT_NULL(root);
    JceJson *arr = jce_json_get(root, "a");
    JceJson *node = jce_json_array_at(arr, 0);
    TEST_ASSERT_NOT_NULL(node);

    jce_json_detach(arr, node);
    TEST_ASSERT_EQUAL_INT(0, jce_json_array_size(arr));
    TEST_ASSERT_EQUAL_STRING("v", jce_json_get_string(node, "k", ""));

    jce_json_free(node);
    jce_json_free(root);
}

static void test_detach_object_member(void)
{
    JceJson *root = jce_json_parse("{\"keep\":1,\"move\":{\"n\":7}}", 0);
    TEST_ASSERT_NOT_NULL(root);
    JceJson *moved = jce_json_get(root, "move");
    TEST_ASSERT_NOT_NULL(moved);

    jce_json_detach(root, moved);
    TEST_ASSERT_FALSE(jce_json_has(root, "move"));
    TEST_ASSERT_TRUE(jce_json_has(root, "keep"));
    TEST_ASSERT_EQUAL_INT(7, jce_json_get_int(moved, "n", -1));

    jce_json_free(moved);
    jce_json_free(root);
}

/* Non-child / NULL arguments must be no-ops, not corruption. */
static void test_detach_tolerates_bad_arguments(void)
{
    JceJson *a = jce_json_parse("{\"x\":[1,2]}", 0);
    JceJson *b = jce_json_parse("{\"y\":[3]}", 0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    JceJson *b_arr = jce_json_get(b, "y");
    JceJson *a_arr = jce_json_get(a, "x");

    jce_json_detach(a_arr, b_arr);          /* not a child of a_arr */
    TEST_ASSERT_EQUAL_INT(2, jce_json_array_size(a_arr));
    TEST_ASSERT_EQUAL_INT(1, jce_json_array_size(b_arr));

    jce_json_detach(NULL, a_arr);
    jce_json_detach(a, NULL);
    jce_json_detach(NULL, NULL);
    TEST_ASSERT_TRUE(jce_json_has(a, "x"));

    jce_json_free(a);
    jce_json_free(b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_object_and_typed_get);
    RUN_TEST(test_parse_array_navigation);
    RUN_TEST(test_parse_invalid_returns_null);
    RUN_TEST(test_build_and_print_roundtrip);
    RUN_TEST(test_get_xyz_with_default);
    RUN_TEST(test_get_xyzw_falls_back_to_default);
    RUN_TEST(test_get_floats_with_default);
    RUN_TEST(test_get_string_and_string_value);
    RUN_TEST(test_any_of_keys);
    RUN_TEST(test_object_iteration_and_member_key);
    RUN_TEST(test_builders_object_array_and_attach);
    RUN_TEST(test_parse_with_explicit_length);
    RUN_TEST(test_parse_file_and_write_file_roundtrip);
    RUN_TEST(test_parse_file_respects_isolated_vfs_miss);
    RUN_TEST(test_type_predicates_and_constructors);
    RUN_TEST(test_detach_moves_node_between_documents);
    RUN_TEST(test_detach_then_free_standalone);
    RUN_TEST(test_detach_object_member);
    RUN_TEST(test_detach_tolerates_bad_arguments);
    return UNITY_END();
}
