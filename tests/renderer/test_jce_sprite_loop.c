/*
 * test_jce_sprite_loop.c — a Loop checkbox that had no door to reach.
 *
 * Looping was a property of the SHEET: JceSpriteAnim.loop comes out of the
 * Aseprite frameTag and jce_sprite_player_update read it directly, while
 * jce_sprite.h offered no player-side setter.  So
 * JceSpriteAnimatorComponent.loop -- authored, serialised, drawn as a
 * checkbox, and defaulted to true by BOTH the parser and the editor's Add
 * Component -- reached nothing at all, in every project.
 *
 * WHY THIS WAS INVISIBLE, twice over.  The loader sets `loop = true` on every
 * tag it reads and on the whole-sheet fallback anim, so essentially everything
 * looped anyway: ticking the box changed nothing, and UNticking it also
 * changed nothing.  And the field gate that should have named it was lending
 * the reader: jce_sr_anim.c also calls a JceSkeletalAnimatorComponent `sa`,
 * so `sa->loop` on THAT pointer counted as a reader for this component until
 * the matching was scoped per function.
 *
 * The sheet here is built in memory rather than loaded from an Aseprite
 * atlas: the point is the player's decision, and a fixture that needs a file
 * on disk would make the failure ambiguous between the two.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/renderer/jce_sprite.h>
#include <jce/resource/jce_atlas_pack.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Two frames, 100 ms each, so three updates of 100 ms walk off the end. */
#define FRAME_MS 100.0f

static char g_atlas[1024];
static char g_dir[1024];

void setUp(void) {}
void tearDown(void) {}

/* An Aseprite-shaped atlas with ONE tag whose direction implies looping --
 * which is what makes "unloop this one instance" a real request rather than a
 * hypothetical: the sheet says loop and the author says once. */
static void write_atlas(void)
{
    FILE *f = fopen(g_atlas, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not write the test atlas");
    fprintf(f,
        "{\"frames\":{"
        "\"a 0.png\":{\"frame\":{\"x\":0,\"y\":0,\"w\":8,\"h\":8},"
        "\"duration\":%d},"
        "\"a 1.png\":{\"frame\":{\"x\":8,\"y\":0,\"w\":8,\"h\":8},"
        "\"duration\":%d}"
        "},\"meta\":{\"image\":\"a.png\",\"frameTags\":["
        "{\"name\":\"walk\",\"from\":0,\"to\":1,\"direction\":\"forward\"}"
        "]}}",
        (int)FRAME_MS, (int)FRAME_MS);
    fclose(f);
}

static JceSpritePlayer *fresh_player(JceSpriteSheet **out_sheet)
{
    JceSpriteSheet *sheet = jce_sprite_sheet_load_json(g_atlas, "a.png");
    TEST_ASSERT_NOT_NULL_MESSAGE(sheet, "the fixture atlas did not load");
    JceSpritePlayer *p = jce_sprite_player_create(sheet);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE_MESSAGE(jce_sprite_player_set_anim(p, "walk"),
        "the fixture's tag is not there, so nothing below is about looping");
    *out_sheet = sheet;
    return p;
}

/* Walk past the end of a two-frame animation. */
static void run_off_the_end(JceSpritePlayer *p)
{
    for (int i = 0; i < 3; ++i)
        jce_sprite_player_update(p, FRAME_MS / 1000.0f, 1.0f);
}

static void test_the_atlas_decides_until_someone_says_otherwise(void)
{
    JceSpriteSheet *sheet;
    JceSpritePlayer *p = fresh_player(&sheet);

    /* THE CONTROL FOR EVERYTHING BELOW.  A player nobody has spoken to must
     * behave exactly as it did before this setter existed, or the two cases
     * after it are measuring a changed default rather than a working
     * override. */
    run_off_the_end(p);
    TEST_ASSERT_FALSE_MESSAGE(jce_sprite_player_is_finished(p),
        "an untouched player must still take the tag's answer, which loops");

    jce_sprite_player_destroy(p);
    jce_sprite_sheet_destroy(sheet);
}

static void test_play_once_overrides_a_looping_tag(void)
{
    JceSpriteSheet *sheet;
    JceSpritePlayer *p = fresh_player(&sheet);

    jce_sprite_player_set_loop(p, 0);
    run_off_the_end(p);
    TEST_ASSERT_TRUE_MESSAGE(jce_sprite_player_is_finished(p),
        "0 means play once; the tag says loop and the caller outranks it");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, jce_sprite_player_current_index(p),
        "a finished animation holds its LAST frame, it does not snap back");

    jce_sprite_player_destroy(p);
    jce_sprite_sheet_destroy(sheet);
}

static void test_turning_looping_back_on_releases_a_finished_player(void)
{
    JceSpriteSheet *sheet;
    JceSpritePlayer *p = fresh_player(&sheet);

    jce_sprite_player_set_loop(p, 0);
    run_off_the_end(p);
    TEST_ASSERT_TRUE(jce_sprite_player_is_finished(p));

    /* THE CASE THAT MAKES THE CHECKBOX HONEST.  Without this, ticking Loop on
     * a finished animation does nothing until the animation is changed -- and
     * "nothing happened" is exactly what an unwired control looks like, which
     * is the defect this whole change exists to close. */
    jce_sprite_player_set_loop(p, 1);
    TEST_ASSERT_FALSE_MESSAGE(jce_sprite_player_is_finished(p),
        "turning Loop on must release a player that already ran out");
    run_off_the_end(p);
    TEST_ASSERT_FALSE_MESSAGE(jce_sprite_player_is_finished(p),
        "...and it must keep looping afterwards, not just unstick once");

    jce_sprite_player_destroy(p);
    jce_sprite_sheet_destroy(sheet);
}

static void test_from_atlas_puts_the_tag_back_in_charge(void)
{
    JceSpriteSheet *sheet;
    JceSpritePlayer *p = fresh_player(&sheet);

    jce_sprite_player_set_loop(p, 0);
    jce_sprite_player_set_loop(p, JCE_SPRITE_LOOP_FROM_ATLAS);
    run_off_the_end(p);
    TEST_ASSERT_FALSE_MESSAGE(jce_sprite_player_is_finished(p),
        "FROM_ATLAS is not a third policy: it hands the decision back");

    jce_sprite_player_destroy(p);
    jce_sprite_sheet_destroy(sheet);
}

/* ---- the packer's output is a sheet this engine can load ------------ */

/* THE CLAIM THE PACKER RESTS ON, closed rather than asserted.
 * jce_atlas_pack exists because the cook could not build a sprite sheet, and
 * its whole design argument is "emit the format the engine already reads, so
 * the runtime half is code that shipped years ago".  That is a claim about
 * two pieces of code agreeing, and the only way to check it is to run one
 * into the other: write with jce_atlas_write_aseprite_json -- the SAME
 * function jce_cook --pack-atlas calls, not a copy of its output -- and load
 * with jce_sprite_sheet_load_json. */
static void test_the_packers_json_loads_as_a_sprite_sheet(void)
{
    JceAtlasItem items[3];
    items[0].id = 0; items[0].w = 16; items[0].h = 24;
    items[1].id = 1; items[1].w = 32; items[1].h = 8;
    items[2].id = 2; items[2].w = 9;  items[2].h = 9;

    JceAtlasPlacement pl[3];
    uint32_t aw = 0, ah = 0;
    TEST_ASSERT_EQUAL_size_t(3u, jce_atlas_pack(items, 3, NULL, pl, &aw, &ah));

    const char *names[3] = { "walk 0.png", "walk 1.png", "idle 0.png" };
    const char *json = "_ut_packed_atlas.json";
    TEST_ASSERT_TRUE_MESSAGE(
        jce_atlas_write_aseprite_json(json, names, pl, 3, aw, ah, "atlas.png"),
        "the writer failed");

    JceSpriteSheet *sheet = jce_sprite_sheet_load_json(json, "atlas.png");
    TEST_ASSERT_NOT_NULL_MESSAGE(sheet,
        "the engine could not load what the packer wrote -- the whole design "
        "argument for emitting Aseprite JSON was that this loader reads it");

    /* And the RECTANGLES survive, not just the parse.  A loader that returned
     * a sheet with three zero-sized frames would satisfy NOT_NULL and draw
     * nothing, which is exactly the kind of pass this tree keeps finding. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, jce_sprite_sheet_frame_count(sheet),
        "the frame count does not match what was written");
    for (uint32_t i = 0; i < 3u; ++i) {
        const JceSpriteFrame *fr = jce_sprite_sheet_get_frame(sheet, i);
        TEST_ASSERT_NOT_NULL(fr);
        TEST_ASSERT_TRUE_MESSAGE(fr->w > 0 && fr->h > 0,
            "a frame came back with no size; the sheet parses and draws "
            "nothing");
        TEST_ASSERT_TRUE_MESSAGE((uint32_t)fr->x + fr->w <= aw &&
                                 (uint32_t)fr->y + fr->h <= ah,
            "a loaded frame falls outside the atlas the packer reported");
    }

    jce_sprite_sheet_destroy(sheet);
    remove(json);
}

int main(void)
{
    if (!jce_sprite_sheet_load_json)  /* silences an unused-symbol warning */
        return 1;
    snprintf(g_dir, sizeof(g_dir), ".");
    snprintf(g_atlas, sizeof(g_atlas), "%s/_ut_sprite_loop.json", g_dir);
    write_atlas();

    UNITY_BEGIN();
    RUN_TEST(test_the_atlas_decides_until_someone_says_otherwise);
    RUN_TEST(test_play_once_overrides_a_looping_tag);
    RUN_TEST(test_turning_looping_back_on_releases_a_finished_player);
    RUN_TEST(test_from_atlas_puts_the_tag_back_in_charge);
    RUN_TEST(test_the_packers_json_loads_as_a_sprite_sheet);
    {
        int rc = UNITY_END();
        remove(g_atlas);
        return rc;
    }
}
