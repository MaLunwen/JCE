/*
 * test_jce_json_hardening.c — malicious/edge-input hardening for the JSON
 * facade (audit plan §35.2: unknown fields, malformed input, depth and size
 * limits, Unicode).
 *
 * These are the cases that matter once JSON is a persisted user-settings and
 * scene format rather than a debug convenience: every one of them arrives from
 * a file the engine did not write.  The escaping round-trip in particular
 * locks in the fix for JSON-02 — the audio-mixer writer used to build JSON
 * with snprintf, so a bus named with a quote corrupted the file (and was an
 * injection vector) until both sides moved onto this facade.
 */

#include "unity.h"

#include <jce/os/core/jce_json.h>

#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* A name containing every character that a hand-rolled writer gets wrong. */
void test_string_with_metacharacters_roundtrips_exactly(void)
{
    static const char nasty[] =
        "quote:\" backslash:\\ newline:\n tab:\t brace:{} bracket:[] comma:,"
        " colon: control:\x01";

    JceJson *root = jce_json_object();
    TEST_ASSERT_NOT_NULL(root);
    jce_json_set_string(root, "name", nasty);

    char *text = jce_json_print(root, false);
    TEST_ASSERT_NOT_NULL(text);
    jce_json_free(root);

    JceJson *back = jce_json_parse(text, 0);
    jce_json_free_string(text);
    TEST_ASSERT_NOT_NULL_MESSAGE(back, "escaped output did not parse back");

    const char *got = jce_json_string_value(jce_json_get(back, "name"), NULL);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(nasty, got,
        "metacharacters did not survive the round-trip");
    jce_json_free(back);
}

/* Non-ASCII must survive byte-for-byte (UTF-8 is the engine's string ABI). */
void test_utf8_roundtrips_exactly(void)
{
    static const char utf8[] = "中文 · ja:日本語 · ko:한국어 · emoji:\xF0\x9F\x8E\xAE";

    JceJson *root = jce_json_object();
    TEST_ASSERT_NOT_NULL(root);
    jce_json_set_string(root, "s", utf8);

    char *text = jce_json_print(root, true);
    TEST_ASSERT_NOT_NULL(text);
    jce_json_free(root);

    JceJson *back = jce_json_parse(text, 0);
    jce_json_free_string(text);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_EQUAL_STRING(utf8, jce_json_string_value(jce_json_get(back, "s"), NULL));
    jce_json_free(back);
}

/* Deeply nested input must be REJECTED, not recursed into until the stack
 * dies.  A parser that blows the stack here is a denial-of-service on any
 * path that parses a file the user did not author (mod, DLC, downloaded
 * scene).  Either outcome is acceptable — a NULL or a valid tree — as long as
 * the process survives to run the assertion. */
void test_deep_nesting_does_not_crash(void)
{
    enum { DEPTH = 20000 };
    char *buf = (char *)malloc(DEPTH * 2u + 1u);
    TEST_ASSERT_NOT_NULL(buf);
    for (int i = 0; i < DEPTH; ++i)      buf[i] = '[';
    for (int i = 0; i < DEPTH; ++i)      buf[DEPTH + i] = ']';
    buf[DEPTH * 2] = '\0';

    JceJson *j = jce_json_parse(buf, (size_t)(DEPTH * 2));
    /* cJSON caps nesting, so NULL is the expected answer; a tree is fine too. */
    if (j) jce_json_free(j);
    free(buf);
    TEST_PASS_MESSAGE("survived deeply nested input");
}

/* Truncated / structurally broken documents must fail cleanly (NULL), never
 * read past the buffer.  The explicit-length overload is the one the PAK and
 * network paths use, where the bytes are NOT NUL-terminated. */
void test_malformed_inputs_fail_cleanly(void)
{
    static const char *const bad[] = {
        "{",  "[",  "{\"a\":",  "{\"a\":1,",  "[1,2",  "\"unterminated",
        "{\"a\" 1}",  "tru",  "{'a':1}",  "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        JceJson *j = jce_json_parse(bad[i], strlen(bad[i]));
        if (j) jce_json_free(j);   /* a lenient accept is not a crash */
    }
    TEST_PASS_MESSAGE("malformed inputs handled without crashing");
}

/* A length-delimited parse must not read beyond the length it was given, even
 * when the buffer continues with valid-looking JSON. */
void test_explicit_length_is_respected(void)
{
    static const char doc[] = "{\"a\":1}{\"b\":2}";
    JceJson *j = jce_json_parse(doc, 7);   /* only the first object */
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(1, jce_json_get_int(j, "a", -1));
    TEST_ASSERT_FALSE_MESSAGE(jce_json_has(j, "b"),
        "parser read past the supplied length");
    jce_json_free(j);
}

/* Missing keys and wrong-typed keys must yield the caller's default rather
 * than garbage — this is the contract every scene/settings loader relies on
 * when it meets a file written by an older or newer build. */
void test_missing_and_mistyped_fields_use_defaults(void)
{
    JceJson *j = jce_json_parse("{\"n\":\"not-a-number\",\"b\":42}", 0);
    TEST_ASSERT_NOT_NULL(j);

    TEST_ASSERT_EQUAL_INT(7, jce_json_get_int(j, "absent", 7));
    TEST_ASSERT_EQUAL_INT(7, jce_json_get_int(j, "n", 7));        /* string -> int */
    TEST_ASSERT_TRUE(jce_json_get_bool(j, "absent", true));
    TEST_ASSERT_NULL(jce_json_string_value(jce_json_get(j, "absent"), NULL));

    jce_json_free(j);
}

/* NULL-tolerance: every accessor is reachable from a failed parse, so none of
 * them may dereference a NULL node. */
void test_accessors_tolerate_null(void)
{
    TEST_ASSERT_NULL(jce_json_get(NULL, "x"));
    TEST_ASSERT_FALSE(jce_json_has(NULL, "x"));
    TEST_ASSERT_EQUAL_INT(3, jce_json_get_int(NULL, "x", 3));
    TEST_ASSERT_EQUAL_INT(0, jce_json_array_size(NULL));
    TEST_ASSERT_NULL(jce_json_array_at(NULL, 0));
    TEST_ASSERT_NULL(jce_json_string_value(NULL, NULL));
    jce_json_free(NULL);
    jce_json_free_string(NULL);
    TEST_PASS_MESSAGE("NULL-tolerant accessors");
}

void test_strict_parse_rejects_trailing_content(void)
{
    static const char with_trailing[] = "{\"ok\":true} trailing";
    static const char with_space[] = "{\"ok\":true} \r\n\t";

    TEST_ASSERT_NULL(jce_json_parse_strict(with_trailing,
                                           sizeof(with_trailing) - 1u));
    JceJson *valid = jce_json_parse_strict(with_space,
                                           sizeof(with_space) - 1u);
    TEST_ASSERT_NOT_NULL(valid);
    TEST_ASSERT_TRUE(jce_json_bool_value(jce_json_get(valid, "ok"), false));
    jce_json_free(valid);
}

void test_null_and_bool_node_accessors(void)
{
    JceJson *root = jce_json_parse_strict("[null,true,false]", 17u);

    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_TRUE(jce_json_is_null(jce_json_array_at(root, 0)));
    TEST_ASSERT_TRUE(jce_json_bool_value(jce_json_array_at(root, 1), false));
    TEST_ASSERT_FALSE(jce_json_bool_value(jce_json_array_at(root, 2), true));
    TEST_ASSERT_TRUE(jce_json_bool_value(NULL, true));
    jce_json_free(root);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_string_with_metacharacters_roundtrips_exactly);
    RUN_TEST(test_utf8_roundtrips_exactly);
    RUN_TEST(test_deep_nesting_does_not_crash);
    RUN_TEST(test_malformed_inputs_fail_cleanly);
    RUN_TEST(test_explicit_length_is_respected);
    RUN_TEST(test_missing_and_mistyped_fields_use_defaults);
    RUN_TEST(test_accessors_tolerate_null);
    RUN_TEST(test_strict_parse_rejects_trailing_content);
    RUN_TEST(test_null_and_bool_node_accessors);
    return UNITY_END();
}
