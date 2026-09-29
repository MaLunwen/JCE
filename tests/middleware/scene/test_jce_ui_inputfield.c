/* test_jce_ui_inputfield.c
 *
 * Single-line text-entry widget — UI InputField.
 *
 * Drives the REAL headless UI canvas pipeline (jce_ui_canvas_render on a
 * canvas created with renderer == NULL) against an authored JceScene that
 * carries a Canvas + a UIInputField at a KNOWN screen rect, then:
 *   - establishes focus with a synthetic JceUIPointer click (press+release
 *     over the field), asserting jce_ui_canvas_focused_input;
 *   - feeds UTF-8 text via jce_ui_canvas_text_input and editing keys via
 *     jce_ui_canvas_key_edit and asserts the canvas MUTATES the component's
 *     `text` in place (Unity source-of-truth model) with the exact expected
 *     strings + caret behaviour.
 *
 * The full layout + raycast + focus/edit/caret logic runs with no live bgfx
 * context (renderer NULL) — only the GPU draws are skipped — so the edit logic
 * is exercised exactly as it runs in a shipped game.  Mirrors the harness of
 * test_jce_ui_widgets.c.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/os/platform/jce_keys.h>

#include "unity.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Shared scene geometry ─────────────────────────────────────────────
 * A 200x40 screen.  The field child uses a fixed-size RectTransform
 * (anchor_min == anchor_max == {0,1}, pivot {0,1} -- UGUI top-left) so uc_resolve_rect places
 * it deterministically at (0,0)..(200,40) — the whole screen rect. */
#define SCREEN_W   200.0f
#define SCREEN_H   40.0f

static void set_full_rect(JceRectTransform *rt, float w, float h)
{
    memset(rt, 0, sizeof *rt);
    /* UGUI: anchor + pivot at the parent TOP-left so the element hangs from
     * the top edge whatever its height.  Spelled out rather than left at the
     * memset zeros -- {0,0} is UGUI BOTTOM-left and only coincided with this
     * while the rect happened to be exactly full height. */
    rt->anchor_min[0] = 0.0f; rt->anchor_min[1] = 1.0f;
    rt->anchor_max[0] = 0.0f; rt->anchor_max[1] = 1.0f;
    rt->pivot[0]      = 0.0f; rt->pivot[1]      = 1.0f;
    rt->size_delta[0] = w; rt->size_delta[1] = h;
}

static JceScene *make_canvas(JceEntity *out_canvas)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity canvas = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    cv.sort_order  = 0;
    jce_scene_set_canvas(s, canvas, &cv);

    *out_canvas = canvas;
    return s;
}

/* Author a Canvas + a child InputField with the given flags. */
static JceScene *make_scene_with_field(JceEntity *out_field, int content_type,
                                       int char_limit, bool is_password,
                                       bool read_only, bool interactable)
{
    JceEntity canvas = 0;
    JceScene *s = make_canvas(&canvas);

    JceEntity field = jce_scene_create_entity(s, "InputField");
    jce_scene_set_parent(s, field, canvas);

    JceUIInputFieldComponent f;
    memset(&f, 0, sizeof f);
    f.content_type = content_type;
    f.char_limit   = char_limit;
    f.is_password  = is_password;
    f.read_only    = read_only;
    f.interactable = interactable;
    f.bg_color[3] = f.text_color[3] = f.caret_color[3] = 1.0f;
    f.font_size = 16.0f;
    set_full_rect(&f.rect, SCREEN_W, SCREEN_H);
    jce_scene_set_ui_input_field(s, field, &f);
    TEST_ASSERT_TRUE(jce_scene_has_ui_input_field(s, field));

    *out_field = field;
    return s;
}

/* Drive one render frame through the REAL canvas pipeline. */
static void step(JceUICanvas *uc, JceScene *s, float x, float y,
                 bool down, bool valid)
{
    JceUIPointer ptr;
    ptr.x = x; ptr.y = y; ptr.down = down; ptr.valid = valid;
    jce_ui_canvas_render(uc, s, /*view*/0, /*fb*/0, SCREEN_W, SCREEN_H,
                         valid ? &ptr : NULL, 1.0f / 60.0f);
}

/* Press + release over the field centre to focus it. */
static void click_field(JceUICanvas *uc, JceScene *s)
{
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;
    step(uc, s, cx, cy, true,  true);   /* press  */
    step(uc, s, cx, cy, false, true);   /* release → focus */
}

static const char *field_text(JceScene *s, JceEntity e)
{
    JceUIInputFieldComponent *f = jce_scene_get_ui_input_field(s, e);
    TEST_ASSERT_NOT_NULL(f);
    return f->text;
}

/* ── 1. focus + type inserts ──────────────────────────────────────────── */
static void test_focus_and_type_inserts(void)
{
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, 0, 0, false, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Not focused before any click. */
    step(uc, s, 0.0f, 0.0f, false, true);
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_focused_input(uc));

    click_field(uc, s);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)field, jce_ui_canvas_focused_input(uc));

    jce_ui_canvas_text_input(uc, "hello");
    TEST_ASSERT_EQUAL_STRING("hello", field_text(s, field));

    /* Caret at end: typing more appends. */
    jce_ui_canvas_text_input(uc, "!");
    TEST_ASSERT_EQUAL_STRING("hello!", field_text(s, field));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 2. backspace / delete / caret move ───────────────────────────────── */
static void test_backspace_delete_caret(void)
{
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, 0, 0, false, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    click_field(uc, s);
    jce_ui_canvas_text_input(uc, "hello");        /* caret at end (5) */

    /* LEFT then BACKSPACE: caret 5→4, delete 'l' before caret → "helo". */
    jce_ui_canvas_key_edit(uc, JCE_KEY_LEFT, 0);
    jce_ui_canvas_key_edit(uc, JCE_KEY_BACKSPACE, 0);
    TEST_ASSERT_EQUAL_STRING("helo", field_text(s, field));

    /* END then type: appends at the end → "helo123". */
    jce_ui_canvas_key_edit(uc, JCE_KEY_END, 0);
    jce_ui_canvas_text_input(uc, "123");
    TEST_ASSERT_EQUAL_STRING("helo123", field_text(s, field));

    /* HOME then DELETE: removes the first char → "elo123". */
    jce_ui_canvas_key_edit(uc, JCE_KEY_HOME, 0);
    jce_ui_canvas_key_edit(uc, JCE_KEY_DELETE, 0);
    TEST_ASSERT_EQUAL_STRING("elo123", field_text(s, field));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 2b. editing steps CHARACTERS, not bytes ──────────────────────────── */
static void test_multibyte_editing_is_not_per_byte(void)
{
    /* Until 2026-09-20 every edit key moved exactly one BYTE, so backspacing
     * a three-byte character removed one third of it and left two
     * continuation bytes behind.  That is a CORRUPTED string, not a wrong
     * one: it has the right length-minus-one, it renders as a replacement
     * glyph, and nothing that counts bytes notices.
     *
     * The assertions below are on the whole string rather than on its length
     * for exactly that reason -- a byte-stepping backspace passes a length
     * check. */
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, 0, 0, false, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    click_field(uc, s);
    /* Three characters, three bytes each. */
    jce_ui_canvas_text_input(uc, "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97");
    TEST_ASSERT_EQUAL_STRING("\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97",
                             field_text(s, field));

    /* BACKSPACE removes the whole last character. */
    jce_ui_canvas_key_edit(uc, JCE_KEY_BACKSPACE, 0);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("\xe4\xb8\xad\xe6\x96\x87",
        field_text(s, field),
        "backspace removed part of a multi-byte character");

    /* HOME + DELETE removes the whole first one. */
    jce_ui_canvas_key_edit(uc, JCE_KEY_HOME, 0);
    jce_ui_canvas_key_edit(uc, JCE_KEY_DELETE, 0);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("\xe6\x96\x87", field_text(s, field),
        "delete removed part of a multi-byte character");

    /* RIGHT then LEFT must land back where it started, so typing there
     * inserts at a character boundary rather than inside one. */
    jce_ui_canvas_key_edit(uc, JCE_KEY_RIGHT, 0);
    jce_ui_canvas_key_edit(uc, JCE_KEY_LEFT, 0);
    jce_ui_canvas_text_input(uc, "x");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("x" "\xe6\x96\x87", field_text(s, field),
        "a caret round trip landed inside a character");

    /* ASCII must be untouched by all of this -- the negative control. */
    jce_ui_canvas_key_edit(uc, JCE_KEY_END, 0);
    jce_ui_canvas_text_input(uc, "ab");
    jce_ui_canvas_key_edit(uc, JCE_KEY_BACKSPACE, 0);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("x" "\xe6\x96\x87" "a", field_text(s, field),
        "single-byte editing changed behaviour");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

static void test_the_byte_buffer_is_never_overrun(void)
{
    /* THE TWO CAPS ARE IN DIFFERENT UNITS AND BOTH ARE LOAD-BEARING.
     *
     * text is char[256]; char_limit 0 means "buffer cap".  If the buffer
     * guard were expressed in CHARACTERS it would admit 255 three-byte
     * characters -- 765 bytes into a 256-byte array.  That mutation survived
     * the first version of these tests, because nothing here filled the
     * field, which is exactly the kind of hole a mutation control exists to
     * find. */
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, 0, /*no limit*/0, false, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    click_field(uc, s);
    for (int i = 0; i < 200; i++)            /* 600 bytes offered */
        jce_ui_canvas_text_input(uc, "\xe4\xb8\xad");

    const char *t = field_text(s, field);
    const int blen = (int)strlen(t);
    TEST_ASSERT_TRUE_MESSAGE(blen <= 255,
        "the field wrote past its 256-byte buffer");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, blen % 3,
        "the buffer filled to a byte count that is not a whole number of "
        "three-byte characters, so the last one was cut in half");
    /* Every byte is either a lead or a continuation, in the right order. */
    for (int i = 0; i < blen; i += 3) {
        TEST_ASSERT_EQUAL_HEX8((unsigned char)0xE4, (unsigned char)t[i]);
        TEST_ASSERT_EQUAL_HEX8((unsigned char)0xB8, (unsigned char)t[i + 1]);
        TEST_ASSERT_EQUAL_HEX8((unsigned char)0xAD, (unsigned char)t[i + 2]);
    }

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

static void test_char_limit_counts_characters_not_bytes(void)
{
    /* jce_scene.h calls char_limit "max chars".  It was enforced in BYTES, so
     * a limit of 4 admitted one three-byte character and then the FIRST BYTE
     * of the next -- a broken string wearing a length limit.  Two characters
     * must fit under a limit of 4 and three must not. */
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, 0, /*limit*/4, false, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    click_field(uc, s);
    jce_ui_canvas_text_input(uc, "\xe4\xb8\xad\xe6\x96\x87");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("\xe4\xb8\xad\xe6\x96\x87", field_text(s, field),
        "two characters did not fit under a four-CHARACTER limit, so the "
        "limit is still counting bytes");

    /* Filling to the limit then one more: the extra is refused WHOLE. */
    jce_ui_canvas_text_input(uc, "ab");
    TEST_ASSERT_EQUAL_STRING("\xe4\xb8\xad\xe6\x96\x87" "ab", field_text(s, field));
    jce_ui_canvas_text_input(uc, "\xe5\xad\x97");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("\xe4\xb8\xad\xe6\x96\x87" "ab", field_text(s, field),
        "a character past the limit was partially inserted");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 3. char_limit caps insertion ─────────────────────────────────────── */
static void test_char_limit(void)
{
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, 0, /*limit*/3, false, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    click_field(uc, s);
    jce_ui_canvas_text_input(uc, "abcdef");
    TEST_ASSERT_EQUAL_STRING("abc", field_text(s, field));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 4. content_type integer filters letters ──────────────────────────── */
static void test_content_type_integer(void)
{
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, /*integer*/1, 0, false, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    click_field(uc, s);
    jce_ui_canvas_text_input(uc, "12a3");
    TEST_ASSERT_EQUAL_STRING("123", field_text(s, field));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 5. read_only ignores input; non-interactable cannot focus ────────── */
static void test_read_only_and_non_interactable(void)
{
    /* read_only: focusable, but text input is a no-op. */
    JceEntity ro = 0;
    JceScene *s1 = make_scene_with_field(&ro, 0, 0, false, /*read_only*/true, true);
    JceUICanvas *uc1 = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc1);
    click_field(uc1, s1);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)ro, jce_ui_canvas_focused_input(uc1));
    jce_ui_canvas_text_input(uc1, "nope");
    TEST_ASSERT_EQUAL_STRING("", field_text(s1, ro));
    jce_ui_canvas_destroy(uc1);
    jce_scene_destroy(s1);

    /* non-interactable: cannot be focused at all. */
    JceEntity ni = 0;
    JceScene *s2 = make_scene_with_field(&ni, 0, 0, false, false, /*inter*/false);
    JceUICanvas *uc2 = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc2);
    click_field(uc2, s2);
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_focused_input(uc2));
    jce_ui_canvas_text_input(uc2, "nope");
    TEST_ASSERT_EQUAL_STRING("", field_text(s2, ni));
    jce_ui_canvas_destroy(uc2);
    jce_scene_destroy(s2);
}

/* ── 6. RETURN submits ────────────────────────────────────────────────── */
static void test_submit(void)
{
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, 0, 0, false, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    click_field(uc, s);
    jce_ui_canvas_text_input(uc, "name");
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_last_submitted(uc));

    jce_ui_canvas_key_edit(uc, JCE_KEY_RETURN, 0);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)field, jce_ui_canvas_last_submitted(uc));
    /* Submit keeps focus + value. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)field, jce_ui_canvas_focused_input(uc));
    TEST_ASSERT_EQUAL_STRING("name", field_text(s, field));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 7. ESCAPE defocuses; later text input is inert ───────────────────── */
static void test_defocus(void)
{
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, 0, 0, false, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    click_field(uc, s);
    jce_ui_canvas_text_input(uc, "abc");
    TEST_ASSERT_EQUAL_STRING("abc", field_text(s, field));

    jce_ui_canvas_key_edit(uc, JCE_KEY_ESCAPE, 0);
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_focused_input(uc));

    /* No field focused → text input is a no-op (value unchanged). */
    jce_ui_canvas_text_input(uc, "xyz");
    TEST_ASSERT_EQUAL_STRING("abc", field_text(s, field));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 8. password masking is value-preserving ──────────────────────────── */
static void test_password_value_preserving(void)
{
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, 0, 0, /*is_password*/true, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    click_field(uc, s);
    jce_ui_canvas_text_input(uc, "pw");
    /* The stored value is the real text; masking is render-only. */
    TEST_ASSERT_EQUAL_STRING("pw", field_text(s, field));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}


/* ── PROBE (temporary, parity audit): non-Latin editing granularity ──── */
#include <stdio.h>
static void dump(const char *tag, const char *s)
{
    printf("PROBE %s: len=%d bytes=[", tag, (int)strlen(s));
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        printf("%02X ", *p);
    printf("]\n");
}
static void test_probe_non_latin(void)
{
    JceEntity field = 0;
    JceScene *s = make_scene_with_field(&field, 0, 0, false, false, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);
    click_field(uc, s);

    /* U+4E2D U+6587 == "zhong wen", 3 bytes each. */
    jce_ui_canvas_text_input(uc, "\xE4\xB8\xAD\xE6\x96\x87");
    dump("after typing 2 CJK chars", field_text(s, field));

    jce_ui_canvas_key_edit(uc, JCE_KEY_BACKSPACE, 0);
    dump("after ONE backspace", field_text(s, field));

    jce_ui_canvas_key_edit(uc, JCE_KEY_LEFT, 0);
    jce_ui_canvas_text_input(uc, "X");
    dump("after LEFT then 'X'", field_text(s, field));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);

    /* char_limit = 4 with two 3-byte characters. */
    JceEntity f2 = 0;
    JceScene *s2 = make_scene_with_field(&f2, 0, /*char_limit*/4, false, false, true);
    JceUICanvas *uc2 = jce_ui_canvas_create(NULL, NULL);
    click_field(uc2, s2);
    jce_ui_canvas_text_input(uc2, "\xE4\xB8\xAD\xE6\x96\x87");
    dump("char_limit=4, typed 2 CJK chars", field_text(s2, f2));
    jce_ui_canvas_destroy(uc2);
    jce_scene_destroy(s2);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_focus_and_type_inserts);
    RUN_TEST(test_backspace_delete_caret);
    RUN_TEST(test_multibyte_editing_is_not_per_byte);
    RUN_TEST(test_the_byte_buffer_is_never_overrun);
    RUN_TEST(test_char_limit_counts_characters_not_bytes);
    RUN_TEST(test_char_limit);
    RUN_TEST(test_content_type_integer);
    RUN_TEST(test_read_only_and_non_interactable);
    RUN_TEST(test_submit);
    RUN_TEST(test_defocus);
    RUN_TEST(test_password_value_preserving);
    RUN_TEST(test_probe_non_latin);
    return UNITY_END();
}
