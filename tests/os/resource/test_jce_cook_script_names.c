/*
 * test_jce_cook_script_names.c — THE COOK TRAP GATE.
 *
 * ══ WHAT WENT WRONG ═══════════════════════════════════════════════════
 *
 * `jce_cook --batch` without `--preserve-names` replaced every asset's
 * extension with `.jceasset`.  For a COOKED asset that is lossless — the
 * container carries a JCEASSET_TYPE_ tag in its own header, so the runtime
 * still knows what it is holding.  A SCRIPT has no container: it ships as its
 * own bytes and its EXTENSION IS THE ENTIRE ROUTING RECORD.  Both authorities
 * that decide which VM runs a file — jce_script_vm_language_for_path() at run
 * time and jce_asset_script_language_from_ext() offline — answer by extension
 * and by nothing else.
 *
 * So `scripts/es_fireflies.py` reached the PAK as `scripts/es_fireflies
 * .jceasset`, which resolves to NO LANGUAGE, and the script died — IN A
 * PACKAGED BUILD ONLY, because the editor keeps reading loose files off disk
 * and never notices.  `jce_add_pak` passes the flag; a hand-rolled cook step
 * does not.  The publication policy dropped `.py` from shipped builds for
 * months in exactly this shape.
 *
 * MEASURED at the parent commit, real jce_cook, six script files in
 * (2026-08-16):
 *
 *     IN : probe.lua  probe.py  Probe.java  Probe.class  Probe.jcecpp  Probe.jcec
 *     OUT: probe.jceasset  probe.jceasset(!)  Probe.jceasset  Probe.jceasset ...
 *
 * Six went in and FIVE came out.  That is the half of the trap nobody had
 * looked for: `Probe.java` and `Probe.class` are two different files, of two
 * different FORMS (source and bytecode), and they collapse onto ONE output
 * name — the loser silently overwritten.  A shipped Java game carries exactly
 * that pair (EsCampfire.java beside EsCampfire.class), so the rename did not
 * merely mislabel the bytecode a Java game runs on, it DELETED it.
 *
 * ══ WHAT THIS GATE ASSERTS, AND WHY IT IS SHAPED THIS WAY ═════════════
 *
 * The acceptance is "a script must never reach a PAK under a name no language
 * claims".  That is a statement about the ARTEFACT, so this test reads the
 * artefact:
 *
 *   - the REAL jce_cook binary produced the cooked tree — not a stub, not a
 *     re-implementation of make_output_path() living in the test;
 *   - the REAL jce_pak binary produced the archive;
 *   - the cook ran DELIBERATELY WITHOUT `--preserve-names`, because a gate
 *     that passes the flag and then checks the flag arrived is testing the
 *     caller, and every caller in this repository already passes it.  The
 *     bug was never in the callers that pass it;
 *   - "does this name resolve to a language" is answered by the ONE
 *     authority, jce_asset_script_language_from_ext() — linked, not parsed.
 *     A copy of the extension table inside this test would be a second
 *     authority that agrees with itself while the engine disagrees.
 *
 * See tests/os/resource/cook_script_names_setup.cmake for the cook+pak driver.
 *
 * ══ THE .json CONTROL ═════════════════════════════════════════════════
 *
 * test_a_non_script_is_still_renamed is not decoration.  There is a fix that
 * makes every assertion above pass and is WRONG: default `--preserve-names`
 * to ON for everything.  That silently changes the name of every cooked asset
 * in every project, and `.jceasset` is a truthful name for a cooked container
 * — the rename is only lossy for the one class of file that has no container.
 * The control is the discriminator between the targeted fix and the blanket
 * one, and it is the assertion that goes red if someone later "simplifies"
 * the rule.
 */

#include "unity.h"

#include <jce/resource/jce_pak_loader.h>
#include <jce/resource/jce_asset_format.h>

#include <stdio.h>
#include <string.h>

#ifndef JCE_COOK_SCRIPT_NAMES_PAK
#  error "JCE_COOK_SCRIPT_NAMES_PAK must name the PAK the setup step produced"
#endif

/* The fixture, and its expected verdict.  Kept in lockstep with the file list
 * in cook_script_names_setup.cmake by test_the_fixture_and_this_table_agree
 * below: a language added to the catalog with a fixture but no row here (or
 * the reverse) fails on the count, not silently on nothing. */
typedef struct {
    const char *vpath;      /* PAK key the cook must have produced      */
    const char *language;   /* what the catalog must call it            */
} ScriptExpectation;

static const ScriptExpectation k_expected[] = {
    { "scripts/probe.lua",    "lua"    },
    { "scripts/probe.py",     "python" },
    { "scripts/Probe.java",   "java"   },
    /* Same stem as the .java on purpose — this pair is the collision. */
    { "scripts/Probe.class",  "java"   },
    { "scripts/Probe.jcecpp", "cpp"    },
    { "scripts/Probe.jcec",   "c"      },
};
#define EXPECTED_COUNT ((int)(sizeof k_expected / sizeof k_expected[0]))

static JcePakArchive *s_pak;

void setUp(void)
{
    s_pak = jce_pak_open_file(JCE_COOK_SCRIPT_NAMES_PAK);
}

void tearDown(void)
{
    if (s_pak) jce_pak_close(s_pak);
    s_pak = NULL;
}

/* Silence compares equal to silence.  Every later test reads entries out of
 * this archive and concludes things from what it finds; if the setup step
 * never ran, an empty archive would make "no script is misnamed" trivially
 * true.  So the archive is proved non-empty on its own before anything is
 * concluded from its contents. */
static void test_the_archive_the_real_tools_produced_exists_and_is_not_empty(void)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(
        s_pak,
        "could not open " JCE_COOK_SCRIPT_NAMES_PAK " — the cook+pak setup "
        "step (cook_script_names_setup) did not run or failed; every "
        "assertion below would have passed vacuously");
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        0u, jce_pak_count(s_pak),
        "the archive opened but holds nothing");
}

/* THE ACCEPTANCE.  Not "the flag was passed" — the outcome: nothing that came
 * out of a script source is sitting in the PAK under a name that resolves to
 * no language. */
static void test_no_script_reaches_the_pak_under_a_name_no_language_claims(void)
{
    uint32_t n, i;
    int      found = 0;

    TEST_ASSERT_NOT_NULL(s_pak);
    n = jce_pak_count(s_pak);

    for (i = 0; i < n; ++i) {
        const JcePakAsset *a = jce_pak_get(s_pak, i);
        TEST_ASSERT_NOT_NULL(a);
        if (strncmp(a->path, "scripts/", 8) != 0) continue;
        ++found;

        if (jce_asset_script_language_from_ext(a->path) == NULL) {
            char msg[512];
            snprintf(msg, sizeof msg,
                     "PAK key '%s' came out of the scripts/ fixture and NO "
                     "language claims its extension. A packaged build will "
                     "load this file and be unable to say what it is; the "
                     "editor will keep working from loose files. The cooker "
                     "renamed a script.",
                     a->path);
            TEST_FAIL_MESSAGE(msg);
        }
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        EXPECTED_COUNT, found,
        "the number of scripts/ keys in the PAK is not the number of script "
        "fixtures the setup step wrote — either two of them collided onto one "
        "output name (the .java/.class case) or the fixture list and this "
        "test's table have drifted apart");
}

/* Each language individually, named.  The loop above proves "no key is
 * unclaimed"; it does NOT prove the key still means what it meant, and a
 * cooker that renamed `probe.py` to `probe.lua` would sail through it. */
static void test_every_language_survives_the_cook_under_its_own_name(void)
{
    int i;
    TEST_ASSERT_NOT_NULL(s_pak);

    for (i = 0; i < EXPECTED_COUNT; ++i) {
        const JcePakAsset *a = jce_pak_find(s_pak, k_expected[i].vpath);
        const char        *lang;
        char               msg[512];

        snprintf(msg, sizeof msg,
                 "PAK has no key '%s' — the cook renamed it, dropped it, or "
                 "another fixture overwrote it",
                 k_expected[i].vpath);
        TEST_ASSERT_NOT_NULL_MESSAGE(a, msg);

        lang = jce_asset_script_language_from_ext(a->path);
        snprintf(msg, sizeof msg,
                 "'%s' survived the cook but the catalog calls it '%s', not "
                 "'%s'",
                 a->path, lang ? lang : "(nothing)", k_expected[i].language);
        TEST_ASSERT_NOT_NULL_MESSAGE(lang, msg);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(k_expected[i].language, lang, msg);
    }
}

/* The half of the trap that is not about names at all.  Two DIFFERENT files
 * of two different forms, one stem: before the fix they became one file and
 * one of them ceased to exist.  Assert both are present AND that the BYTES
 * that come back out differ — "both keys exist" would also be true if the
 * packer had pointed both at one blob, and each fixture file states which
 * one it is in its own text, so a survivor can be identified by name. */
static void test_a_java_source_and_its_class_do_not_collide(void)
{
    const JcePakAsset *src;
    const JcePakAsset *cls;
    char               src_bytes[256];
    char               cls_bytes[256];
    size_t             ns, nc;

    TEST_ASSERT_NOT_NULL(s_pak);
    src = jce_pak_find(s_pak, "scripts/Probe.java");
    cls = jce_pak_find(s_pak, "scripts/Probe.class");

    TEST_ASSERT_NOT_NULL_MESSAGE(
        src, "scripts/Probe.java is not in the PAK");
    TEST_ASSERT_NOT_NULL_MESSAGE(
        cls,
        "scripts/Probe.class is not in the PAK — a shipped Java game runs on "
        "the BYTECODE, so this is the file whose loss ends the game, and the "
        "rename made it share an output name with its own source");

    TEST_ASSERT_TRUE(src->original_size < sizeof src_bytes);
    TEST_ASSERT_TRUE(cls->original_size < sizeof cls_bytes);
    ns = jce_pak_decompress(src, src_bytes, sizeof src_bytes);
    nc = jce_pak_decompress(cls, cls_bytes, sizeof cls_bytes);

    /* Each side independently produced output before either is compared:
     * two failed decompressions both yield zero bytes, and zero bytes
     * compare equal to zero bytes. */
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, (int)ns,
        "scripts/Probe.java decompressed to nothing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, (int)nc,
        "scripts/Probe.class decompressed to nothing");

    src_bytes[ns] = '\0';
    cls_bytes[nc] = '\0';
    TEST_ASSERT_TRUE_MESSAGE(
        strstr(src_bytes, "SOURCE-SIDE") != NULL,
        "the bytes under scripts/Probe.java are not the java SOURCE fixture "
        "— the class file overwrote it");
    TEST_ASSERT_TRUE_MESSAGE(
        strstr(cls_bytes, "BYTECODE-SIDE") != NULL,
        "the bytes under scripts/Probe.class are not the bytecode fixture — "
        "the java source overwrote it");
}

/* The discriminator described in the file header: the rule is "scripts are
 * never renamed", NOT "nothing is ever renamed". */
static void test_a_non_script_is_still_renamed(void)
{
    TEST_ASSERT_NOT_NULL(s_pak);

    TEST_ASSERT_NOT_NULL_MESSAGE(
        jce_pak_find(s_pak, "data/table.jceasset"),
        "the non-script control did not come out as data/table.jceasset — if "
        "the fix was 'default --preserve-names to ON', it silently changed "
        "the name of every cooked asset in every project instead of the one "
        "class of file the rename actually destroys");
    TEST_ASSERT_NULL_MESSAGE(
        jce_pak_find(s_pak, "data/table.json"),
        "data/table.json kept its name, so the cooker stopped renaming "
        "everything, not just scripts");
}

/* THE SECOND NAMING SITE.  jce_cook names an output twice: once for the file
 * it is writing, and once in the per-run stale sweep that deletes the output
 * of a source that has since been deleted.  Both must use one rule.
 *
 * MEASURED: with the sweep left on the old `if (preserve_names)` and the
 * write site fixed, the whole suite was GREEN — this assertion is the only
 * thing that fails, and without it the comment at that site would have been
 * one more contract nothing enforces.  The consequence is not cosmetic: the
 * sweep hunts `scripts/doomed.jceasset`, never finds it, and the script the
 * author deleted keeps shipping in every build from then on.
 *
 * The setup step writes doomed.lua, cooks, deletes it, and cooks again. */
static void test_a_deleted_script_stops_shipping(void)
{
    TEST_ASSERT_NOT_NULL(s_pak);
    TEST_ASSERT_NULL_MESSAGE(
        jce_pak_find(s_pak, "scripts/doomed.lua"),
        "the source scripts/doomed.lua was deleted before the second cook and "
        "its cooked copy is STILL IN THE PAK — the cooker's stale sweep is "
        "naming outputs by a different rule than the one it writes them "
        "under, so a removed script ships forever");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_archive_the_real_tools_produced_exists_and_is_not_empty);
    RUN_TEST(test_no_script_reaches_the_pak_under_a_name_no_language_claims);
    RUN_TEST(test_every_language_survives_the_cook_under_its_own_name);
    RUN_TEST(test_a_java_source_and_its_class_do_not_collide);
    RUN_TEST(test_a_non_script_is_still_renamed);
    RUN_TEST(test_a_deleted_script_stops_shipping);
    return UNITY_END();
}
