/*
 * test_jce_material_inherit.c -- a .mat.json that names a parent.
 *
 * The GPU measurement says an empty child renders pixel-identically to its
 * parent while an empty material with NO parent does not.  What it cannot say
 * is any of the following, each of which produces a plausible picture in the
 * one case a screenshot happens to cover:
 *
 *   omitted keys inherit ..... the loader's defaults used to be LITERALS
 *                              (metallic 0.0, roughness 1.0, white base
 *                              colour).  With a parent in place a literal is
 *                              not "the default", it is "discard what the
 *                              parent said" -- and it would still render, just
 *                              as the wrong material.
 *   stated keys override ..... the other half of the same rule.  A child that
 *                              inherits EVERYTHING is not a variant.
 *   chains resolve ........... grandparent -> parent -> child.  A one-level
 *                              implementation looks correct until the second
 *                              level exists.
 *   cycles do not hang ....... a <-> b is a file somebody will write, and the
 *                              natural implementation recurses forever.
 *   SAVE keeps the link ...... every caller rewrites the whole file from a
 *                              JcePbrMaterial, which has no parent field.  If
 *                              the key is not preserved the first Save turns
 *                              the variant into an unrelated copy that still
 *                              looks like a variant.
 *   SAVE stores overrides .... and only overrides.  Write the resolved value
 *                              set instead and editing the parent stops
 *                              reaching the child: a feature that works once.
 *   the PARENT ships ......... the bundle packer learns that a JSON string
 *                              names a file from a table, and "parent" does
 *                              not look like a path.  Leave it out and the
 *                              built game renders the child's overrides over
 *                              the DEFAULTS -- right in the editor, wrong in
 *                              the exe, and no screenshot of the editor can
 *                              ever show it.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_bundle_deps.h>
#include <jce/renderer/jce_pbr_material.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, ...)                                                      \
    do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__);           \
                        printf(__VA_ARGS__); printf("\n"); g_fail = 1; } }    \
    while (0)

#define DIR "jce_mat_inherit_tmp"

static void wr(const char *name, const char *json)
{
    char p[256];
    snprintf(p, sizeof p, DIR "/%s", name);
    if (!jce_fs_host_write_all(p, json, strlen(json))) {
        printf("FAIL: cannot write %s\n", p);
        g_fail = 1;
    }
}

static bool ld(const char *name, JcePbrMaterial *m, char tex[5][256])
{
    char p[256];
    snprintf(p, sizeof p, DIR "/%s", name);
    return jce_pbr_material_load_json(p, m, tex);
}

static char *slurp(const char *name)
{
    char p[256];
    snprintf(p, sizeof p, DIR "/%s", name);
    uint64_t sz = 0;
    return (char *)jce_fs_host_read_all(p, &sz);
}

static void rm(const char *name)
{
    char p[256];
    snprintf(p, sizeof p, DIR "/%s", name);
    jce_fs_host_remove_file(p);
}

int main(void)
{
    jce_fs_host_create_directory(DIR);

    /* A parent that differs from the defaults in EVERY field the child could
     * silently reset -- otherwise "the child inherited" and "the child fell
     * back to the default" are the same picture. */
    wr("p.mat.json",
       "{\"type\":\"pbr\","
       "\"baseColorFactor\":[0.9,0.08,0.05,1.0],"
       "\"emissiveFactor\":[0.35,0.02,0.0],"
       "\"metallicFactor\":0.75,"
       "\"roughnessFactor\":0.35,"
       "\"normalScale\":2.0,"
       "\"aoStrength\":0.25,"
       "\"alphaCutoff\":0.125,"
       "\"renderPriority\":7,"
       "\"doubleSided\":true,"
       "\"alphaMode\":\"MASK\","
       "\"albedoTexture\":\"tex/p_albedo.png\"}");

    JcePbrMaterial P, C;
    char ptex[5][256], ctex[5][256];
    CHECK(ld("p.mat.json", &P, ptex), "parent must load");
    CHECK(P.metallic_factor == 0.75f, "parent metallic %f", (double)P.metallic_factor);

    /* ── an empty child is its parent ──────────────────────────────── */
    wr("c_empty.mat.json", "{\"type\":\"pbr\",\"parent\":\"p.mat.json\"}");
    CHECK(ld("c_empty.mat.json", &C, ctex), "empty child must load");
    CHECK(C.base_color_factor[0] == P.base_color_factor[0] &&
          C.base_color_factor[1] == P.base_color_factor[1] &&
          C.base_color_factor[2] == P.base_color_factor[2] &&
          C.base_color_factor[3] == P.base_color_factor[3],
          "base colour not inherited: %f %f %f",
          (double)C.base_color_factor[0], (double)C.base_color_factor[1],
          (double)C.base_color_factor[2]);
    CHECK(C.emissive_factor[0] == P.emissive_factor[0],
          "emissive not inherited: %f", (double)C.emissive_factor[0]);
    CHECK(C.metallic_factor  == P.metallic_factor,  "metallic not inherited: %f",
          (double)C.metallic_factor);
    CHECK(C.roughness_factor == P.roughness_factor, "roughness not inherited: %f",
          (double)C.roughness_factor);
    CHECK(C.normal_scale     == P.normal_scale,     "normalScale not inherited");
    CHECK(C.ao_strength      == P.ao_strength,      "aoStrength not inherited");
    CHECK(C.alpha_cutoff     == P.alpha_cutoff,     "alphaCutoff not inherited");
    CHECK(C.render_priority  == P.render_priority,  "renderPriority not inherited");
    CHECK(C.double_sided     == P.double_sided,     "doubleSided not inherited");
    CHECK(C.alpha_mode       == P.alpha_mode,       "alphaMode not inherited");
    CHECK(strcmp(ctex[0], ptex[0]) == 0,
          "albedo path not inherited: '%s' vs '%s'", ctex[0], ptex[0]);

    /* ── a stated key overrides, and ONLY it ───────────────────────── */
    wr("c_one.mat.json",
       "{\"type\":\"pbr\",\"parent\":\"p.mat.json\",\"metallicFactor\":0.125}");
    CHECK(ld("c_one.mat.json", &C, ctex), "override child must load");
    CHECK(C.metallic_factor == 0.125f, "override lost: %f", (double)C.metallic_factor);
    CHECK(C.roughness_factor == P.roughness_factor,
          "one override reset roughness to %f (the literal-default bug)",
          (double)C.roughness_factor);
    CHECK(C.base_color_factor[0] == P.base_color_factor[0],
          "one override reset the base colour to %f",
          (double)C.base_color_factor[0]);

    /* ── chains ────────────────────────────────────────────────────── */
    wr("g.mat.json",
       "{\"type\":\"pbr\",\"parent\":\"p.mat.json\",\"roughnessFactor\":0.9}");
    wr("gc.mat.json",
       "{\"type\":\"pbr\",\"parent\":\"g.mat.json\",\"aoStrength\":0.5}");
    CHECK(ld("gc.mat.json", &C, ctex), "grandchild must load");
    CHECK(C.ao_strength      == 0.5f, "own key lost: %f", (double)C.ao_strength);
    CHECK(C.roughness_factor == 0.9f, "middle level lost: %f",
          (double)C.roughness_factor);
    CHECK(C.metallic_factor  == P.metallic_factor,
          "grandparent lost: %f", (double)C.metallic_factor);

    /* ── a cycle is refused, not followed ──────────────────────────── */
    wr("a.mat.json", "{\"type\":\"pbr\",\"parent\":\"b.mat.json\","
                     "\"metallicFactor\":0.5}");
    wr("b.mat.json", "{\"type\":\"pbr\",\"parent\":\"a.mat.json\","
                     "\"roughnessFactor\":0.5}");
    CHECK(ld("a.mat.json", &C, ctex), "a cycle must still LOAD, not hang");
    CHECK(C.metallic_factor == 0.5f, "a's own key survived the cycle: %f",
          (double)C.metallic_factor);

    /* Self-parent is the degenerate case and has its own guard. */
    wr("self.mat.json",
       "{\"type\":\"pbr\",\"parent\":\"self.mat.json\",\"aoStrength\":0.75}");
    CHECK(ld("self.mat.json", &C, ctex), "self-parent must load");
    CHECK(C.ao_strength == 0.75f, "self-parent lost its own key");

    /* A parent that does not exist must not turn the child into defaults. */
    wr("orphan.mat.json",
       "{\"type\":\"pbr\",\"parent\":\"nope.mat.json\",\"metallicFactor\":0.6}");
    CHECK(ld("orphan.mat.json", &C, ctex), "orphan must load");
    CHECK(C.metallic_factor == 0.6f, "orphan lost its own key: %f",
          (double)C.metallic_factor);

    /* ── the parent API ────────────────────────────────────────────── */
    {
        char got[256];
        char p[256];
        snprintf(p, sizeof p, DIR "/c_one.mat.json");
        CHECK(jce_pbr_material_get_parent(p, got, sizeof got),
              "get_parent must find one");
        CHECK(strcmp(got, "p.mat.json") == 0, "get_parent: '%s'", got);

        snprintf(p, sizeof p, DIR "/p.mat.json");
        CHECK(!jce_pbr_material_get_parent(p, got, sizeof got),
              "a root material has no parent, got '%s'", got);

        CHECK(!jce_pbr_material_set_parent(p, p),
              "a material must not be allowed to be its own parent");
    }

    /* ── SAVE keeps the link and stores only overrides ─────────────── */
    {
        char cp[256];
        snprintf(cp, sizeof cp, DIR "/c_one.mat.json");
        JcePbrMaterial m;
        char tex[5][256];
        CHECK(jce_pbr_material_load_json(cp, &m, tex), "reload for save");
        /* Change ONE more thing, then write the whole resolved material back
         * exactly as the inspector's Save does. */
        m.ao_strength = 0.875f;
        CHECK(jce_pbr_material_save_json(cp, &m, tex), "save must succeed");

        char *txt = slurp("c_one.mat.json");
        CHECK(txt != NULL, "saved file must be readable");
        if (txt) {
            CHECK(strstr(txt, "\"parent\"") != NULL,
                  "SAVE DROPPED THE PARENT LINK:\n%s", txt);
            CHECK(strstr(txt, "aoStrength") != NULL,
                  "the changed value was not written:\n%s", txt);
            CHECK(strstr(txt, "metallicFactor") != NULL,
                  "the pre-existing override was not kept:\n%s", txt);
            CHECK(strstr(txt, "roughnessFactor") == NULL,
                  "a value equal to the parent's was written anyway -- the\n"
                  "child is now a snapshot and editing the parent reaches\n"
                  "nothing:\n%s", txt);
            CHECK(strstr(txt, "baseColorFactor") == NULL,
                  "base colour equal to the parent's was written:\n%s", txt);
            jce_fs_buffer_free(txt);
        }

        /* And the round trip still resolves to the same material. */
        JcePbrMaterial back;
        char btex[5][256];
        CHECK(jce_pbr_material_load_json(cp, &back, btex), "reload after save");
        CHECK(back.ao_strength     == 0.875f, "ao lost in the round trip: %f",
              (double)back.ao_strength);
        CHECK(back.metallic_factor == 0.125f, "override lost: %f",
              (double)back.metallic_factor);
        CHECK(back.roughness_factor == P.roughness_factor,
              "inherited value lost: %f", (double)back.roughness_factor);

        /* THE POINT OF THE WHOLE FEATURE: edit the parent, and the saved
         * child follows.  This is what a flattened save would break. */
        wr("p.mat.json",
           "{\"type\":\"pbr\","
           "\"baseColorFactor\":[0.1,0.2,0.9,1.0],"
           "\"roughnessFactor\":0.05,"
           "\"metallicFactor\":0.75}");
        CHECK(jce_pbr_material_load_json(cp, &back, btex), "reload after edit");
        CHECK(back.roughness_factor == 0.05f,
              "the parent's new roughness did not reach the saved child: %f",
              (double)back.roughness_factor);
        CHECK(back.base_color_factor[2] == 0.9f,
              "the parent's new base colour did not reach the child: %f",
              (double)back.base_color_factor[2]);
        CHECK(back.metallic_factor == 0.125f,
              "the child's own override was overwritten by the parent: %f",
              (double)back.metallic_factor);
    }

    /* ── the SHIPPED path: the parent must travel with the child ──
     *
     * The bundle packer learns that a JSON string names a file from
     * kAssetKeys[].  "parent" does not look like a path, so the key that most
     * needs collecting is the one most likely to be left out -- and the
     * failure is silent: the shipped game loads the child, cannot find the
     * parent, and renders the child's overrides on top of the DEFAULTS.
     * Right in the editor, wrong in the exe. */
    {
        static const char *const child =
            "{\"type\":\"pbr\",\"parent\":\"p.mat.json\","
            "\"albedoMap\":\"tex/c.png\"}";
        JceBundleDepList deps;
        memset(&deps, 0, sizeof deps);
        CHECK(jce_bundle_deps_scan(child, strlen(child), &deps),
              "deps scan must succeed");
        bool saw_parent = false, saw_tex = false;
        for (uint32_t i = 0; i < deps.count; i++) {
            if (strcmp(deps.items[i].path, "p.mat.json") == 0) saw_parent = true;
            if (strcmp(deps.items[i].path, "tex/c.png")  == 0) saw_tex = true;
        }
        CHECK(saw_parent,
              "THE PACKER WILL NOT SHIP THE PARENT: a variant in the built "
              "game renders its overrides over the DEFAULTS");
        CHECK(saw_tex, "the texture key regressed too (%u dep(s))",
              (unsigned)deps.count);
        jce_bundle_deps_free(&deps);
    }

    for (int i = 0; i < 9; i++) {
        static const char *const names[9] = {
            "p.mat.json", "c_empty.mat.json", "c_one.mat.json", "g.mat.json",
            "gc.mat.json", "a.mat.json", "b.mat.json", "self.mat.json",
            "orphan.mat.json"
        };
        rm(names[i]);
    }

    printf(g_fail ? "test_jce_material_inherit: FAILED\n"
                  : "test_jce_material_inherit: OK\n");
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
