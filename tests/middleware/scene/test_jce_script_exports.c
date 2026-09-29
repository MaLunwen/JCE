/*
 * test_jce_script_exports.c
 *
 * A SCRIPT declaring what it wants from an author -- Unity's
 * [SerializeField], Godot's @export, UE's UPROPERTY(EditAnywhere).
 *
 * Both scripting.inspector.exposed-fields and
 * editor.inspector.script-exposed-fields were behind for one shared reason:
 * the Inspector showed what the AUTHOR declared by typing a name and picking
 * a kind, and the script itself had no say.  This is the reader that gives it
 * one.
 *
 * WHAT THE LOAD-BEARING CASES ARE.  Not "it finds a declaration" -- a scanner
 * that returns everything it sees passes that.  The two that carry this file:
 *
 *   a MERGE must not destroy an authored value.  The editor calls it on every
 *   sync, so an overwrite would silently discard tuning as a side effect of
 *   redrawing a component.  Asserted with a value the declaration's default
 *   would visibly replace.
 *
 *   a MALFORMED declaration must produce NO parameter rather than a
 *   parameter with an invented name.  A scanner that guesses turns an
 *   author's typo into a row they did not write, which is worse than the row
 *   being missing: they cannot tell it from one they did.
 *
 * SEVEN LANGUAGES, ONE READER.  The marker is found inside whatever comment
 * syntax surrounds it, so the same line works in lua (--), python (#) and the
 * five that use //.  One case runs all three spellings through it, because
 * "works in the language I tested" is exactly the failure a seven-backend
 * engine keeps paying for.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include <jce/middleware/scene/jce_script_exports.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define MAXP JCE_SCRIPT_PARAM_MAX

static int scan(const char *src, JceScriptParam *out)
{
    memset(out, 0, sizeof(JceScriptParam) * MAXP);
    return jce_script_exports_scan(src, strlen(src), out, MAXP);
}

static const JceScriptParam *find(const JceScriptParam *p, int n, const char *name)
{
    for (int i = 0; i < n; ++i)
        if (strcmp(p[i].name, name) == 0) return &p[i];
    return NULL;
}

/* ── the four kinds ─────────────────────────────────────────────────── */

static void test_every_kind_is_read_with_its_default(void)
{
    JceScriptParam d[MAXP];
    const int n = scan(
        "-- @export number speed = 5.5\n"
        "-- @export bool   alive = true\n"
        "-- @export text   label = patrol\n"
        "-- @export entity target = 7\n", d);
    TEST_ASSERT_EQUAL_INT(4, n);

    const JceScriptParam *s = find(d, n, "speed");
    TEST_ASSERT_NOT_NULL_MESSAGE(s, "the number declaration was not found");
    TEST_ASSERT_EQUAL_UINT32(JCE_SCRIPT_PARAM_NUMBER, s->kind);
    TEST_ASSERT_EQUAL_FLOAT(5.5f, s->number);

    const JceScriptParam *a = find(d, n, "alive");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT32(JCE_SCRIPT_PARAM_BOOL, a->kind);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, a->number,
        "a bool declared `true` did not come back as 1");

    const JceScriptParam *l = find(d, n, "label");
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT32(JCE_SCRIPT_PARAM_TEXT, l->kind);
    TEST_ASSERT_EQUAL_STRING("patrol", l->text);

    const JceScriptParam *t = find(d, n, "target");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT32(JCE_SCRIPT_PARAM_ENTITY, t->kind);
    TEST_ASSERT_EQUAL_UINT64(7u, t->entity);
}

/* A declaration with NO default is still a declaration; its slot is the same
 * zero an unauthored parameter has. */
static void test_a_declaration_without_a_default_is_still_declared(void)
{
    JceScriptParam d[MAXP];
    const int n = scan("// @export number range\n", d);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("range", d[0].name);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d[0].number);
}

/* ── one reader for seven languages ─────────────────────────────────── */

static void test_the_marker_is_found_in_every_comment_syntax(void)
{
    JceScriptParam d[MAXP];
    const int n = scan(
        "-- @export number lua_one = 1\n"          /* lua                    */
        "// @export number slash_one = 2\n"        /* c/cpp/cs/java/js       */
        "#  @export number hash_one = 3\n"         /* python                 */
        "   @export number bare_one = 4\n", d);    /* no comment at all      */
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, n,
        "the same declaration was not read in all four spellings -- the whole "
        "reason this is a comment marker is that seven backends share ONE "
        "reader, so a syntax-sensitive scanner defeats the design");
    TEST_ASSERT_NOT_NULL(find(d, n, "lua_one"));
    TEST_ASSERT_NOT_NULL(find(d, n, "slash_one"));
    TEST_ASSERT_NOT_NULL(find(d, n, "hash_one"));
    TEST_ASSERT_NOT_NULL(find(d, n, "bare_one"));
}

/* `speed=5` and `speed = 5` must parse the same.  Without the '=' terminator
 * in the name scan, the first form yields the name "speed=5", fails
 * validation and vanishes -- an author would see their parameter disappear
 * for a reason nothing reports. */
static void test_spaces_around_equals_do_not_matter(void)
{
    JceScriptParam d[MAXP];
    const int n = scan(
        "// @export number tight=1.25\n"
        "// @export number loose   =   1.25\n", d);
    TEST_ASSERT_EQUAL_INT(2, n);
    const JceScriptParam *a = find(d, n, "tight");
    const JceScriptParam *b = find(d, n, "loose");
    TEST_ASSERT_NOT_NULL_MESSAGE(a, "`name=value` with no spaces was dropped");
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_FLOAT(1.25f, a->number);
    TEST_ASSERT_EQUAL_FLOAT(1.25f, b->number);
}

/* ── malformed input produces nothing, not garbage ──────────────────── */

static void test_a_malformed_declaration_yields_no_parameter(void)
{
    JceScriptParam d[MAXP];

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, scan("// @export flaot speed = 1\n", d),
        "a misspelled KIND produced a parameter; an author cannot tell an "
        "invented row from one they wrote");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, scan("// @export number\n", d),
        "a declaration with no NAME produced a parameter");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, scan("// @export number 9lives = 1\n", d),
        "a name starting with a digit was accepted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, scan("// @export number sp-eed = 1\n", d),
        "a name with punctuation was accepted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
        scan("// @export number this_name_is_far_too_long_to_fit_in_the_field = 1\n", d),
        "an over-long name was accepted, so it was truncated -- and a "
        "truncated name binds to a DIFFERENT parameter than the author meant");

    /* POSITIVE CONTROL: the same shapes with one thing corrected must all
     * produce exactly one.  Without it, a scanner that returns 0 for
     * everything passes every assertion above. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, scan("// @export number speed = 1\n", d),
        "the corrected declaration was ALSO rejected, so the cases above say "
        "nothing about malformed input");
}

/* An unparseable DEFAULT keeps the declaration and zeroes the slot: the
 * intent to expose the parameter is unambiguous even when the value is not. */
static void test_a_bad_default_keeps_the_declaration(void)
{
    JceScriptParam d[MAXP];
    const int n = scan("// @export number speed = fast\n", d);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n,
        "a typo in the DEFAULT made the whole parameter disappear");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d[0].number);
}

static void test_the_first_declaration_of_a_name_wins(void)
{
    JceScriptParam d[MAXP];
    const int n = scan(
        "// @export number speed = 1\n"
        "// @export number speed = 2\n", d);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n, "a duplicate name produced two rows");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, d[0].number,
        "the SECOND declaration won, so jce.get_param would depend on scan "
        "order");
}

/* The component array is bounded; the scanner must stop at the bound rather
 * than write past it. */
static void test_the_scan_stops_at_the_caller_s_bound(void)
{
    JceScriptParam d[MAXP];
    char src[1024];
    src[0] = '\0';
    for (int i = 0; i < MAXP + 4; ++i) {
        char line[64];
        snprintf(line, sizeof line, "// @export number p%d = %d\n", i, i);
        strncat(src, line, sizeof src - strlen(src) - 1);
    }
    const int n = scan(src, d);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAXP, n,
        "the scanner did not stop at max_out");
}

/* ── the merge, which is where an author's work can be lost ─────────── */

static void test_a_merge_keeps_the_authored_value(void)
{
    JceScriptParam decls[MAXP];
    const int dn = scan("// @export number speed = 5.0\n", decls);
    TEST_ASSERT_EQUAL_INT(1, dn);

    /* The author tuned it to 42, which is NOT the declared default -- so if
     * the merge overwrote, the assertion below sees 5.0 and says so. */
    JceScriptParam params[MAXP];
    memset(params, 0, sizeof params);
    snprintf(params[0].name, sizeof params[0].name, "speed");
    params[0].kind   = JCE_SCRIPT_PARAM_NUMBER;
    params[0].number = 42.0f;

    const int n = jce_script_exports_merge(decls, dn, params, 1, MAXP);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n, "the merge duplicated an existing name");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(42.0f, params[0].number,
        "the merge replaced an AUTHORED value with the script's default -- "
        "the editor calls this on every sync, so tuning would be discarded as "
        "a side effect of redrawing the component");
}

static void test_a_merge_adds_what_is_declared_and_keeps_what_is_not(void)
{
    JceScriptParam decls[MAXP];
    const int dn = scan("// @export number added = 3\n", decls);

    JceScriptParam params[MAXP];
    memset(params, 0, sizeof params);
    snprintf(params[0].name, sizeof params[0].name, "stale");
    params[0].number = 9.0f;

    const int n = jce_script_exports_merge(decls, dn, params, 1, MAXP);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n, "the declared parameter was not added");

    const JceScriptParam *a = find(params, n, "added");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, a->number);

    const JceScriptParam *s = find(params, n, "stale");
    TEST_ASSERT_NOT_NULL_MESSAGE(s,
        "a parameter the script no longer declares was DELETED -- a script "
        "mid-rename would silently destroy a value that cannot be recovered "
        "from the script file");
    TEST_ASSERT_EQUAL_FLOAT(9.0f, s->number);

    /* And the editor can tell them apart, which is the only way an author
     * sees which row is left over. */
    TEST_ASSERT_TRUE(jce_script_exports_declares(decls, dn, "added"));
    TEST_ASSERT_FALSE(jce_script_exports_declares(decls, dn, "stale"));
}

/* A declaration whose KIND changed must retake the kind, because the script
 * is what decides what the value means. */
static void test_a_merge_takes_the_kind_from_the_script(void)
{
    JceScriptParam decls[MAXP];
    const int dn = scan("// @export text mode = idle\n", decls);

    JceScriptParam params[MAXP];
    memset(params, 0, sizeof params);
    snprintf(params[0].name, sizeof params[0].name, "mode");
    params[0].kind = JCE_SCRIPT_PARAM_NUMBER;

    jce_script_exports_merge(decls, dn, params, 1, MAXP);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(JCE_SCRIPT_PARAM_TEXT, params[0].kind,
        "the script changed the parameter's kind and the component kept the "
        "old one, so the Inspector would draw the wrong widget");
}

/* The merge must not run past the component's fixed array. */
static void test_a_merge_stops_at_the_capacity(void)
{
    JceScriptParam decls[MAXP];
    char src[1024];
    src[0] = '\0';
    for (int i = 0; i < MAXP; ++i) {
        char line[64];
        snprintf(line, sizeof line, "// @export number d%d = %d\n", i, i);
        strncat(src, line, sizeof src - strlen(src) - 1);
    }
    const int dn = scan(src, decls);
    TEST_ASSERT_EQUAL_INT(MAXP, dn);

    JceScriptParam params[MAXP];
    memset(params, 0, sizeof params);
    snprintf(params[0].name, sizeof params[0].name, "occupied");

    const int n = jce_script_exports_merge(decls, dn, params, 1, MAXP);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAXP, n, "the merge exceeded the capacity");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("occupied", params[0].name,
        "the pre-existing parameter was overwritten while filling up");
}

static void test_bad_arguments_are_refused(void)
{
    JceScriptParam d[MAXP];
    TEST_ASSERT_EQUAL_INT(-1, jce_script_exports_scan(NULL, 0, d, MAXP));
    TEST_ASSERT_EQUAL_INT(-1, jce_script_exports_scan("x", 1, NULL, MAXP));
    TEST_ASSERT_EQUAL_INT(-1, jce_script_exports_scan("x", 1, d, 0));
    TEST_ASSERT_EQUAL_INT(-1, jce_script_exports_merge(d, 1, NULL, 0, MAXP));
    TEST_ASSERT_FALSE(jce_script_exports_declares(NULL, 0, "x"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_kind_is_read_with_its_default);
    RUN_TEST(test_a_declaration_without_a_default_is_still_declared);
    RUN_TEST(test_the_marker_is_found_in_every_comment_syntax);
    RUN_TEST(test_spaces_around_equals_do_not_matter);
    RUN_TEST(test_a_malformed_declaration_yields_no_parameter);
    RUN_TEST(test_a_bad_default_keeps_the_declaration);
    RUN_TEST(test_the_first_declaration_of_a_name_wins);
    RUN_TEST(test_the_scan_stops_at_the_caller_s_bound);
    RUN_TEST(test_a_merge_keeps_the_authored_value);
    RUN_TEST(test_a_merge_adds_what_is_declared_and_keeps_what_is_not);
    RUN_TEST(test_a_merge_takes_the_kind_from_the_script);
    RUN_TEST(test_a_merge_stops_at_the_capacity);
    RUN_TEST(test_bad_arguments_are_refused);
    return UNITY_END();
}
