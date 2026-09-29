/*
 * test_jce_material_stencil.c -- the material's bgfx stencil word.
 *
 * The GPU measurement says the word reaches the pixels (NEVER removed 43.35%
 * of the frame, ALWAYS was pixel-identical to no stencil at all).  What a
 * screenshot cannot say is whether each individual value maps to the bgfx
 * constant that MEANS it, because most wrong mappings still produce A picture:
 *
 *   OFF is not a compare ..... a zeroed material must emit NO stencil word.
 *                              Emit one and every existing draw starts testing
 *                              against a buffer nobody wrote -- and the result
 *                              depends on what the last pass left there.
 *   each compare is itself ... LESS and LEQUAL differ by one pixel at the
 *                              boundary, which is exactly the pixel a portal
 *                              edge or an outline is made of.
 *   the three ops are three .. fail / zfail / pass live in three different bit
 *                              fields.  Swap two and the buffer is written on
 *                              the wrong condition, which shows up only where
 *                              geometry overlaps.
 *   clamp is not wrap ........ INCR saturates at 255, INCR_WRAP rolls to 0.  A
 *                              stack of 255 nested portals is not the test
 *                              case; a mis-mapped op is.
 *   read mask 0 ............. is what a memset holds, not what an author
 *                             means.  Honoured literally it tests no bits at
 *                             all, so every compare returns the same answer
 *                             everywhere and the feature reads as dead.
 *
 * There is no write mask on purpose: bgfx's stencil word has none.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include <jce/renderer/jce_pbr_material.h>

#include <bgfx/c99/bgfx.h>

#include <stdio.h>
#include <stdlib.h>

static int g_fail = 0;

#define CHECK(cond, ...)                                                      \
    do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__);           \
                        printf(__VA_ARGS__); printf("\n"); g_fail = 1; } }    \
    while (0)

static JcePbrMaterial mat_off(void)
{
    JcePbrMaterial m = jce_pbr_material_default();
    return m;
}

int main(void)
{
    /* ── OFF emits nothing ─────────────────────────────────────────── */
    {
        JcePbrMaterial m = mat_off();
        CHECK(m.stencil_func == JCE_STENCIL_OFF,
              "default material must be stencil-OFF, got %u", m.stencil_func);
        CHECK(jce_pbr_material_stencil(&m) == 0u,
              "OFF must emit BGFX_STENCIL_NONE, got 0x%08X",
              jce_pbr_material_stencil(&m));
        CHECK(jce_pbr_material_stencil(NULL) == 0u, "NULL must emit none");

        /* A ref value and ops set while the test is OFF stay inert: that is
         * the state the Inspector leaves behind when an author turns the
         * feature off again, and it must not half-arm the draw. */
        m.stencil_ref = 7;
        m.stencil_pass_op = JCE_STENCIL_OP_REPLACE;
        CHECK(jce_pbr_material_stencil(&m) == 0u,
              "OFF with a ref and an op must still emit none, got 0x%08X",
              jce_pbr_material_stencil(&m));
    }

    /* ── every compare maps to its own bgfx constant ───────────────── */
    {
        const struct { uint8_t func; uint32_t bits; const char *name; } T[] = {
            { JCE_STENCIL_NEVER,    BGFX_STENCIL_TEST_NEVER,    "NEVER"    },
            { JCE_STENCIL_LESS,     BGFX_STENCIL_TEST_LESS,     "LESS"     },
            { JCE_STENCIL_LEQUAL,   BGFX_STENCIL_TEST_LEQUAL,   "LEQUAL"   },
            { JCE_STENCIL_EQUAL,    BGFX_STENCIL_TEST_EQUAL,    "EQUAL"    },
            { JCE_STENCIL_GEQUAL,   BGFX_STENCIL_TEST_GEQUAL,   "GEQUAL"   },
            { JCE_STENCIL_GREATER,  BGFX_STENCIL_TEST_GREATER,  "GREATER"  },
            { JCE_STENCIL_NOTEQUAL, BGFX_STENCIL_TEST_NOTEQUAL, "NOTEQUAL" },
            { JCE_STENCIL_ALWAYS,   BGFX_STENCIL_TEST_ALWAYS,   "ALWAYS"   },
        };
        for (int i = 0; i < 8; i++) {
            JcePbrMaterial m = mat_off();
            m.stencil_func = T[i].func;
            uint32_t w = jce_pbr_material_stencil(&m);
            CHECK((w & BGFX_STENCIL_TEST_MASK) == T[i].bits,
                  "%s: test bits 0x%08X, want 0x%08X",
                  T[i].name, w & BGFX_STENCIL_TEST_MASK, T[i].bits);
            /* Distinctness, stated as its own assertion: a table that mapped
             * two entries to the same constant would satisfy every check
             * above for one of them and still be wrong. */
            for (int j = 0; j < i; j++)
                CHECK(T[i].bits != T[j].bits,
                      "%s and %s map to the same bits", T[i].name, T[j].name);
        }
    }

    /* ── ref and read mask ride their own fields ───────────────────── */
    {
        JcePbrMaterial m = mat_off();
        m.stencil_func = JCE_STENCIL_EQUAL;
        m.stencil_ref = 0xA5;
        m.stencil_read_mask = 0x0F;
        uint32_t w = jce_pbr_material_stencil(&m);
        CHECK((w & BGFX_STENCIL_FUNC_REF_MASK) == BGFX_STENCIL_FUNC_REF(0xA5),
              "ref: 0x%08X", w & BGFX_STENCIL_FUNC_REF_MASK);
        CHECK((w & BGFX_STENCIL_FUNC_RMASK_MASK) == BGFX_STENCIL_FUNC_RMASK(0x0F),
              "rmask: 0x%08X", w & BGFX_STENCIL_FUNC_RMASK_MASK);

        /* 0 is the memset value, not an authored one. */
        m.stencil_read_mask = 0;
        w = jce_pbr_material_stencil(&m);
        CHECK((w & BGFX_STENCIL_FUNC_RMASK_MASK) == BGFX_STENCIL_FUNC_RMASK(0xFF),
              "read mask 0 must read as 0xFF, got 0x%08X",
              w & BGFX_STENCIL_FUNC_RMASK_MASK);
    }

    /* ── the three operations land in three different fields ───────── */
    {
        JcePbrMaterial m = mat_off();
        m.stencil_func = JCE_STENCIL_ALWAYS;
        m.stencil_fail_op  = JCE_STENCIL_OP_ZERO;
        m.stencil_zfail_op = JCE_STENCIL_OP_INVERT;
        m.stencil_pass_op  = JCE_STENCIL_OP_REPLACE;
        uint32_t w = jce_pbr_material_stencil(&m);
        CHECK((w & BGFX_STENCIL_OP_FAIL_S_MASK) == BGFX_STENCIL_OP_FAIL_S_ZERO,
              "fail op: 0x%08X", w & BGFX_STENCIL_OP_FAIL_S_MASK);
        CHECK((w & BGFX_STENCIL_OP_FAIL_Z_MASK) == BGFX_STENCIL_OP_FAIL_Z_INVERT,
              "zfail op: 0x%08X", w & BGFX_STENCIL_OP_FAIL_Z_MASK);
        CHECK((w & BGFX_STENCIL_OP_PASS_Z_MASK) == BGFX_STENCIL_OP_PASS_Z_REPLACE,
              "pass op: 0x%08X", w & BGFX_STENCIL_OP_PASS_Z_MASK);
    }

    /* ── clamp and wrap are different operations ───────────────────── */
    {
        JcePbrMaterial a = mat_off(), b = mat_off();
        a.stencil_func = b.stencil_func = JCE_STENCIL_ALWAYS;
        a.stencil_pass_op = JCE_STENCIL_OP_INCR;
        b.stencil_pass_op = JCE_STENCIL_OP_INCR_WRAP;
        CHECK(jce_pbr_material_stencil(&a) != jce_pbr_material_stencil(&b),
              "INCR and INCR_WRAP produced the same word");
        CHECK((jce_pbr_material_stencil(&a) & BGFX_STENCIL_OP_PASS_Z_MASK)
                  == BGFX_STENCIL_OP_PASS_Z_INCRSAT,
              "INCR must be the SATURATING bgfx op");
        CHECK((jce_pbr_material_stencil(&b) & BGFX_STENCIL_OP_PASS_Z_MASK)
                  == BGFX_STENCIL_OP_PASS_Z_INCR,
              "INCR_WRAP must be the WRAPPING bgfx op");

        a.stencil_pass_op = JCE_STENCIL_OP_DECR;
        b.stencil_pass_op = JCE_STENCIL_OP_DECR_WRAP;
        CHECK(jce_pbr_material_stencil(&a) != jce_pbr_material_stencil(&b),
              "DECR and DECR_WRAP produced the same word");
        CHECK((jce_pbr_material_stencil(&a) & BGFX_STENCIL_OP_PASS_Z_MASK)
                  == BGFX_STENCIL_OP_PASS_Z_DECRSAT,
              "DECR must be the SATURATING bgfx op");
    }

    /* ── KEEP is the zero op, so a half-filled block writes nothing ── */
    {
        JcePbrMaterial m = mat_off();
        m.stencil_func = JCE_STENCIL_EQUAL;
        uint32_t w = jce_pbr_material_stencil(&m);
        CHECK((w & BGFX_STENCIL_OP_FAIL_S_MASK) == BGFX_STENCIL_OP_FAIL_S_KEEP,
              "default fail op must be KEEP");
        CHECK((w & BGFX_STENCIL_OP_FAIL_Z_MASK) == BGFX_STENCIL_OP_FAIL_Z_KEEP,
              "default zfail op must be KEEP");
        CHECK((w & BGFX_STENCIL_OP_PASS_Z_MASK) == BGFX_STENCIL_OP_PASS_Z_KEEP,
              "default pass op must be KEEP");
    }

    printf(g_fail ? "test_jce_material_stencil: FAILED\n"
                  : "test_jce_material_stencil: OK\n");
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
