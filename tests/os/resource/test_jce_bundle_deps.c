/* test_jce_bundle_deps.c
 *
 * Unit tests for the scene-to-asset dependency scanner.  These tests
 * are pure (no FS access) — they feed JSON strings into
 * jce_bundle_deps_scan and assert on the discovered references and
 * override tags.
 */

#include <jce/resource/jce_bundle_deps.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* Small helper: does the list contain `path`? */
static int has_path(const JceBundleDepList *l, const char *path)
{
    for (uint32_t i = 0; i < l->count; ++i)
        if (l->items[i].path && strcmp(l->items[i].path, path) == 0)
            return 1;
    return 0;
}

/* Lookup the bundle override for a given path (NULL if not present
 * or no tag attached). */
static const char *bundle_for(const JceBundleDepList *l, const char *path)
{
    for (uint32_t i = 0; i < l->count; ++i)
        if (l->items[i].path && strcmp(l->items[i].path, path) == 0)
            return l->items[i].bundle;
    return NULL;
}

/* ----------------------------------------------------------------- */
/* jce_bundle_deps_is_asset_key                                       */
/* ----------------------------------------------------------------- */

static void test_is_asset_key_known(void)
{
    TEST_ASSERT_TRUE (jce_bundle_deps_is_asset_key("meshPath"));
    TEST_ASSERT_TRUE (jce_bundle_deps_is_asset_key("texturePath"));
    TEST_ASSERT_TRUE (jce_bundle_deps_is_asset_key("audioPath"));
    TEST_ASSERT_TRUE (jce_bundle_deps_is_asset_key("clipPath"));
    TEST_ASSERT_TRUE (jce_bundle_deps_is_asset_key("layerAlbedoPath2"));
    TEST_ASSERT_TRUE (jce_bundle_deps_is_asset_key("layerNormalPath2"));
    TEST_ASSERT_TRUE (jce_bundle_deps_is_asset_key("layerMaskPath2"));
    TEST_ASSERT_TRUE (jce_bundle_deps_is_asset_key("bakedPlacementPath"));
}

static void test_is_asset_key_indexed_mesh(void)
{
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("meshPath0"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("meshPath42"));
    /* trailing non-digit makes it unrecognised */
    TEST_ASSERT_FALSE(jce_bundle_deps_is_asset_key("meshPathX"));
    TEST_ASSERT_FALSE(jce_bundle_deps_is_asset_key("meshPath1a"));
}

static void test_is_asset_key_unknown(void)
{
    TEST_ASSERT_FALSE(jce_bundle_deps_is_asset_key(NULL));
    TEST_ASSERT_FALSE(jce_bundle_deps_is_asset_key(""));
    TEST_ASSERT_FALSE(jce_bundle_deps_is_asset_key("random"));
    TEST_ASSERT_FALSE(jce_bundle_deps_is_asset_key("name"));
}

static void test_is_asset_key_component_coverage(void)
{
    /* MeshRenderer per-entity texture overrides. */
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("albedoTex"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("mrTex"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("normalTex"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("aoTex"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("emissiveTex"));
    /* Lights / physics / particles / AI / anim / cinematics / 2D. */
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("cookiePath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("iesPath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("physMaterial"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("modelPath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("assetPath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("particlePath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("treePath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("stateMachine"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("seqPath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("avatarPath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("maskPath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("overrideController"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("tilemapPath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("spritesPath"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("bakedCubemapPath"));
    /* Material-descriptor texture maps (packer descriptor recursion). */
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("albedoMap"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("baseColorMap"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("diffuseMap"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("mainTexture"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("metallicRoughnessMap"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("metallicMap"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("normalMap"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("aoMap"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("occlusionMap"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("emissiveMap"));
    TEST_ASSERT_TRUE(jce_bundle_deps_is_asset_key("emissionMap"));
    /* Contextual descriptor keys must NOT be global. */
    TEST_ASSERT_FALSE(jce_bundle_deps_is_asset_key("texture"));
    TEST_ASSERT_FALSE(jce_bundle_deps_is_asset_key("sprites"));
    TEST_ASSERT_FALSE(jce_bundle_deps_is_asset_key("source"));
}

/* ----------------------------------------------------------------- */
/* jce_bundle_deps_scan                                               */
/* ----------------------------------------------------------------- */

static void test_scan_null_args(void)
{
    JceBundleDepList l = {0};
    TEST_ASSERT_FALSE(jce_bundle_deps_scan(NULL, 0, &l));
    TEST_ASSERT_FALSE(jce_bundle_deps_scan("{}",  0, NULL));
}

static void test_scan_invalid_json(void)
{
    JceBundleDepList l = {0};
    TEST_ASSERT_FALSE(jce_bundle_deps_scan("{ not json", 0, &l));
}

static void test_scan_empty_object(void)
{
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan("{}", 0, &l));
    TEST_ASSERT_EQUAL_UINT32(0, l.count);
    jce_bundle_deps_free(&l);
}

static void test_scan_single_asset(void)
{
    const char *j =
        "{ \"entities\": [ { \"mesh\": { \"meshPath\": \"models/foo.obj\" } } ] }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(1, l.count);
    TEST_ASSERT_TRUE(has_path(&l, "models/foo.obj"));
    /* No bundle override expected. */
    TEST_ASSERT_NULL(bundle_for(&l, "models/foo.obj"));
    jce_bundle_deps_free(&l);
}

static void test_scan_cooked_foliage_placement(void)
{
    const char *j =
        "{ \"components\": [ { \"type\": \"VegetationScatter\","
        " \"bakedPlacementPath\":"
        " \"generated/foliage/trees.foliage.bin\" } ] }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(1, l.count);
    TEST_ASSERT_TRUE(has_path(&l,
        "generated/foliage/trees.foliage.bin"));
    jce_bundle_deps_free(&l);
}

static void test_scan_dedup(void)
{
    /* Two components referencing the SAME asset must produce a single
     * dep entry (the scanner deduplicates by path). */
    const char *j =
        "{ \"a\": { \"texturePath\": \"tex/x.png\" },"
        "  \"b\": { \"texturePath\": \"tex/x.png\" } }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(1, l.count);
    jce_bundle_deps_free(&l);
}

static void test_scan_normalises_backslashes(void)
{
    /* Windows-style paths in JSON must be normalised to '/'. */
    const char *j =
        "{ \"mat\": { \"materialPath\": \"mats\\\\stone.mat.json\" } }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(1, l.count);
    TEST_ASSERT_TRUE(has_path(&l, "mats/stone.mat.json"));
    jce_bundle_deps_free(&l);
}

static void test_scan_bundle_override_tag(void)
{
    /* The "bundle" sibling string attaches an override tag to every
     * asset key inside the same object. */
    const char *j =
        "{ \"comp\": {"
        "   \"bundle\": \"force-shared\","
        "   \"texturePath\": \"tex/shared.png\""
        "} }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(1, l.count);
    const char *tag = bundle_for(&l, "tex/shared.png");
    TEST_ASSERT_NOT_NULL(tag);
    TEST_ASSERT_EQUAL_STRING("force-shared", tag);
    jce_bundle_deps_free(&l);
}

static void test_scan_terrain_sidecar(void)
{
    /* A .terrain.json reference auto-pulls the matching .terrain.bin. */
    const char *j =
        "{ \"t\": { \"terrainPath\": \"world/area01.terrain.json\" } }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(2, l.count);
    TEST_ASSERT_TRUE(has_path(&l, "world/area01.terrain.json"));
    TEST_ASSERT_TRUE(has_path(&l, "world/area01.terrain.bin"));
    jce_bundle_deps_free(&l);
}

static void test_scan_terrain_material_layers(void)
{
    const char *j =
        "{ \"terrain\": {"
        "  \"layerAlbedoPath0\": \"terrain\\\\soil_a.png\","
        "  \"layerAlbedoPath1\": \"terrain/rock_a.png\","
        "  \"layerAlbedoPath2\": \"terrain/snow_a.png\","
        "  \"layerAlbedoPath3\": \"terrain/moss_a.png\","
        "  \"layerNormalPath0\": \"terrain\\\\soil_n.png\","
        "  \"layerNormalPath1\": \"terrain/rock_n.png\","
        "  \"layerNormalPath2\": \"terrain/snow_n.png\","
        "  \"layerNormalPath3\": \"terrain/moss_n.png\","
        "  \"layerMaskPath0\": \"terrain\\\\soil_m.png\","
        "  \"layerMaskPath1\": \"terrain/rock_m.png\","
        "  \"layerMaskPath2\": \"terrain/snow_m.png\","
        "  \"layerMaskPath3\": \"\""
        "} }";
    JceBundleDepList l = {0};

    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(11, l.count);
    TEST_ASSERT_TRUE(has_path(&l, "terrain/soil_a.png"));
    TEST_ASSERT_TRUE(has_path(&l, "terrain/moss_a.png"));
    TEST_ASSERT_TRUE(has_path(&l, "terrain/soil_n.png"));
    TEST_ASSERT_TRUE(has_path(&l, "terrain/moss_n.png"));
    TEST_ASSERT_TRUE(has_path(&l, "terrain/soil_m.png"));
    TEST_ASSERT_TRUE(has_path(&l, "terrain/snow_m.png"));
    jce_bundle_deps_free(&l);
}

static void test_scan_indexed_mesh_paths(void)
{
    const char *j =
        "{ \"lod\": {"
        "   \"meshPath0\": \"m/a.obj\","
        "   \"meshPath1\": \"m/b.obj\","
        "   \"meshPath2\": \"m/c.obj\""
        "} }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(3, l.count);
    TEST_ASSERT_TRUE(has_path(&l, "m/a.obj"));
    TEST_ASSERT_TRUE(has_path(&l, "m/c.obj"));
    jce_bundle_deps_free(&l);
}

static void test_scan_ignores_empty_strings(void)
{
    const char *j = "{ \"x\": { \"meshPath\": \"\" } }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(0, l.count);
    jce_bundle_deps_free(&l);
}

static void test_scan_uses_strlen_when_len_zero(void)
{
    /* Passing json_len=0 is documented to fall back to strlen.  Verify
     * we still get the asset back. */
    const char *j = "{ \"k\": { \"audioPath\": \"snd/jump.ogg\" } }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_TRUE(has_path(&l, "snd/jump.ogg"));
    jce_bundle_deps_free(&l);
}

static void test_free_zero_initialised_list_is_safe(void)
{
    JceBundleDepList l = {0};
    jce_bundle_deps_free(&l);   /* must not crash */
    jce_bundle_deps_free(NULL); /* tolerant of NULL */
}

static void test_scan_component_paths(void)
{
    /* One synthetic scene exercising the component keys that were
     * previously invisible to the scanner. */
    const char *j =
        "{ \"entities\": [ { \"components\": ["
        "  { \"type\": \"MeshRenderer\", \"albedoTex\": \"tex/a.png\","
        "    \"normalTex\": \"tex/n.png\" },"
        "  { \"type\": \"Light\", \"cookiePath\": \"tex/cookie.png\","
        "    \"iesPath\": \"ies/spot.ies\" },"
        "  { \"type\": \"Rigidbody\", \"physMaterial\": \"phys/ice.physmat.json\" },"
        "  { \"type\": \"ParticleEmitter\", \"assetPath\": \"fx/p.particles.json\" },"
        "  { \"type\": \"BehaviorTree\", \"treePath\": \"ai/guard.bt.json\" },"
        "  { \"type\": \"SkeletalAnimator\", \"skeletonPath\": \"models/hero.glb\","
        "    \"stateMachine\": \"anim/loco.anim_sm.json\" },"
        "  { \"type\": \"SequencePlayer\", \"seqPath\": \"cine/intro.seq.json\" },"
        "  { \"type\": \"CompoundCollider\", \"modelPath\": \"models/crate.glb\" },"
        "  { \"type\": \"ReflectionProbe\", \"bakedCubemapPath\": \"probes/p0.hdr\" },"
        "  { \"type\": \"Tilemap\", \"properties\": {"
        "      \"tilemapPath\": \"maps/t.tilemap.json\","
        "      \"spritesPath\": \"maps/ts.sprites.json\" } },"
        "  { \"type\": \"Avatar\", \"properties\": {"
        "      \"avatarPath\": \"anim/h.avatar.json\","
        "      \"maskPath\": \"anim/upper.mask.json\" } }"
        "] } ] }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_TRUE(has_path(&l, "tex/a.png"));
    TEST_ASSERT_TRUE(has_path(&l, "tex/n.png"));
    TEST_ASSERT_TRUE(has_path(&l, "tex/cookie.png"));
    TEST_ASSERT_TRUE(has_path(&l, "ies/spot.ies"));
    TEST_ASSERT_TRUE(has_path(&l, "phys/ice.physmat.json"));
    TEST_ASSERT_TRUE(has_path(&l, "fx/p.particles.json"));
    TEST_ASSERT_TRUE(has_path(&l, "ai/guard.bt.json"));
    TEST_ASSERT_TRUE(has_path(&l, "models/hero.glb"));
    TEST_ASSERT_TRUE(has_path(&l, "anim/loco.anim_sm.json"));
    TEST_ASSERT_TRUE(has_path(&l, "cine/intro.seq.json"));
    TEST_ASSERT_TRUE(has_path(&l, "models/crate.glb"));
    TEST_ASSERT_TRUE(has_path(&l, "probes/p0.hdr"));
    TEST_ASSERT_TRUE(has_path(&l, "maps/t.tilemap.json"));
    TEST_ASSERT_TRUE(has_path(&l, "maps/ts.sprites.json"));
    TEST_ASSERT_TRUE(has_path(&l, "anim/h.avatar.json"));
    TEST_ASSERT_TRUE(has_path(&l, "anim/upper.mask.json"));
    jce_bundle_deps_free(&l);
}

static void test_scan_material_descriptor_keys(void)
{
    /* A .mat.json document (re-scanned by the packer's descriptor
     * recursion): both engine-primary and alias texture-map keys. */
    const char *j =
        "{ \"properties\": {"
        "   \"albedoMap\": \"tex/albedo.png\","
        "   \"metallicRoughnessMap\": \"tex/mr.png\","
        "   \"normalMap\": \"tex/nrm.png\","
        "   \"occlusionMap\": \"tex/ao.png\","
        "   \"emissionMap\": \"tex/em.png\""
        "} }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(5, l.count);
    TEST_ASSERT_TRUE(has_path(&l, "tex/albedo.png"));
    TEST_ASSERT_TRUE(has_path(&l, "tex/mr.png"));
    TEST_ASSERT_TRUE(has_path(&l, "tex/nrm.png"));
    TEST_ASSERT_TRUE(has_path(&l, "tex/ao.png"));
    TEST_ASSERT_TRUE(has_path(&l, "tex/em.png"));
    jce_bundle_deps_free(&l);
}

static void test_scan_particles_texture_contextual(void)
{
    /* "texture" is harvested only with a particle-descriptor signature
     * sibling (emitRate / lifetimeMin / maxParticles / sizeStart). */
    const char *with_sig =
        "{ \"emitRate\": 24, \"texture\": \"tex/spark.png\" }";
    const char *without_sig =
        "{ \"texture\": \"tex/ignored.png\" }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(with_sig, 0, &l));
    TEST_ASSERT_TRUE(has_path(&l, "tex/spark.png"));
    jce_bundle_deps_free(&l);
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(without_sig, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(0, l.count);
    jce_bundle_deps_free(&l);
}

static void test_scan_tilemap_descriptor_contextual(void)
{
    /* .tilemap.json: "sprites" needs "cells" (or "w"+"h").
     * .sprites.json: "source" needs "rects". */
    const char *tilemap =
        "{ \"w\": 4, \"h\": 4, \"cells\": [0,1,2,3],"
        "  \"sprites\": \"maps/ts.sprites.json\" }";
    const char *tileset =
        "{ \"source\": \"tex/tiles.png\", \"sourceW\": 64, \"sourceH\": 64,"
        "  \"rects\": [ { \"x\": 0, \"y\": 0, \"w\": 16, \"h\": 16 } ] }";
    const char *bare =
        "{ \"sprites\": \"x.json\", \"source\": \"y.png\" }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(tilemap, 0, &l));
    TEST_ASSERT_TRUE(has_path(&l, "maps/ts.sprites.json"));
    jce_bundle_deps_free(&l);
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(tileset, 0, &l));
    TEST_ASSERT_TRUE(has_path(&l, "tex/tiles.png"));
    jce_bundle_deps_free(&l);
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(bare, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(0, l.count);
    jce_bundle_deps_free(&l);
}

/* ----------------------------------------------------------------- */
/* Explicit dependency documents                                     */
/* ----------------------------------------------------------------- */

static void test_dependency_document_parses_required_optional_labels(void)
{
    const char *json =
        "{"
        "  \"contract\": {"
        "    \"name\": \"jce.bundle.dependencies\","
        "    \"major\": 1, \"minor\": 0"
        "  },"
        "  \"assets\": ["
        "    \"audio/thunder.mp3\","
        "    { \"path\": \"textures\\\\season.png\","
        "      \"bundle\": \"force-shared\" }"
        "  ],"
        "  \"optional_assets\": [\"audio/ambience.ogg\"],"
        "  \"labels\": [\"runtime\", \"weather\"]"
        "}";
    JceBundleDependencyDocument doc = {0};

    TEST_ASSERT_TRUE(jce_bundle_deps_parse_document(json, 0, &doc));
    TEST_ASSERT_EQUAL_UINT32(2, doc.assets.count);
    TEST_ASSERT_TRUE(has_path(&doc.assets, "audio/thunder.mp3"));
    TEST_ASSERT_TRUE(has_path(&doc.assets, "textures/season.png"));
    TEST_ASSERT_EQUAL_STRING("force-shared",
        bundle_for(&doc.assets, "textures/season.png"));
    TEST_ASSERT_EQUAL_UINT32(1, doc.optional_assets.count);
    TEST_ASSERT_TRUE(has_path(&doc.optional_assets, "audio/ambience.ogg"));
    TEST_ASSERT_EQUAL_UINT32(2, doc.label_count);
    TEST_ASSERT_EQUAL_STRING("runtime", doc.labels[0]);
    TEST_ASSERT_EQUAL_STRING("weather", doc.labels[1]);

    jce_bundle_deps_document_free(&doc);
}

static void test_dependency_document_rejects_wrong_contract(void)
{
    const char *json =
        "{ \"contract\": { \"name\": \"jce.bundle\","
        "  \"major\": 1, \"minor\": 0 }, \"assets\": [] }";
    JceBundleDependencyDocument doc = {0};

    TEST_ASSERT_FALSE(jce_bundle_deps_parse_document(json, 0, &doc));
    jce_bundle_deps_document_free(&doc);
}

static void test_dependency_document_rejects_invalid_entries(void)
{
    const char *bad_assets =
        "{ \"contract\": { \"name\": \"jce.bundle.dependencies\","
        "  \"major\": 1, \"minor\": 0 }, \"assets\": [7] }";
    const char *bad_optional =
        "{ \"contract\": { \"name\": \"jce.bundle.dependencies\","
        "  \"major\": 1, \"minor\": 0 }, \"assets\": [],"
        "  \"optional_assets\": \"audio/a.ogg\" }";
    JceBundleDependencyDocument doc = {0};

    TEST_ASSERT_FALSE(jce_bundle_deps_parse_document(bad_assets, 0, &doc));
    jce_bundle_deps_document_free(&doc);
    TEST_ASSERT_FALSE(jce_bundle_deps_parse_document(bad_optional, 0, &doc));
    jce_bundle_deps_document_free(&doc);
}

/* ----------------------------------------------------------------- */

/* A Shader Graph material ships its blobs -- and one per BACKEND.
 *
 * Both halves of "a graph material rendered in the editor and reverted to
 * stock PBR when shipped" are asserted here: the packer did not collect
 * customProgramVs/Fs at all, and the bare filename the .mat.json stores names
 * bytecode for whatever backend the EDITOR was running, so shipping only that
 * one link-fails everywhere else. */
static void test_scan_shader_graph_blobs(void)
{
    const char *j =
        "{ \"customProgramVs\": \"vs_water.bin\","
        "  \"customProgramFs\": \"fs_water.bin\" }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    /* The names the material actually stores. */
    TEST_ASSERT_TRUE(has_path(&l, "vs_water.bin"));
    TEST_ASSERT_TRUE(has_path(&l, "fs_water.bin"));
    /* ...and every backend variant beside them, same suffixes the engine's
     * own shaders use. */
    TEST_ASSERT_TRUE(has_path(&l, "vs_water_dx11.bin"));
    TEST_ASSERT_TRUE(has_path(&l, "vs_water_spv.bin"));
    TEST_ASSERT_TRUE(has_path(&l, "vs_water_glsl.bin"));
    TEST_ASSERT_TRUE(has_path(&l, "vs_water_essl.bin"));
    TEST_ASSERT_TRUE(has_path(&l, "vs_water_mtl.bin"));
    TEST_ASSERT_TRUE(has_path(&l, "fs_water_dx11.bin"));
    TEST_ASSERT_TRUE(has_path(&l, "fs_water_glsl.bin"));
    jce_bundle_deps_free(&l);
}

/* The expansion is for graph blobs, not for every asset key: a mesh path must
 * not sprout six imaginary siblings. */
static void test_scan_backend_expansion_is_only_for_graph_blobs(void)
{
    const char *j = "{ \"meshPath\": \"models/foo.bin\" }";
    JceBundleDepList l = {0};
    TEST_ASSERT_TRUE(jce_bundle_deps_scan(j, 0, &l));
    TEST_ASSERT_EQUAL_UINT32(1, l.count);
    TEST_ASSERT_FALSE(has_path(&l, "models/foo_dx11.bin"));
    jce_bundle_deps_free(&l);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_is_asset_key_known);
    RUN_TEST(test_is_asset_key_indexed_mesh);
    RUN_TEST(test_is_asset_key_unknown);
    RUN_TEST(test_is_asset_key_component_coverage);
    RUN_TEST(test_scan_null_args);
    RUN_TEST(test_scan_invalid_json);
    RUN_TEST(test_scan_empty_object);
    RUN_TEST(test_scan_single_asset);
    RUN_TEST(test_scan_cooked_foliage_placement);
    RUN_TEST(test_scan_dedup);
    RUN_TEST(test_scan_normalises_backslashes);
    RUN_TEST(test_scan_bundle_override_tag);
    RUN_TEST(test_scan_terrain_sidecar);
    RUN_TEST(test_scan_terrain_material_layers);
    RUN_TEST(test_scan_indexed_mesh_paths);
    RUN_TEST(test_scan_ignores_empty_strings);
    RUN_TEST(test_scan_uses_strlen_when_len_zero);
    RUN_TEST(test_free_zero_initialised_list_is_safe);
    RUN_TEST(test_scan_component_paths);
    RUN_TEST(test_scan_material_descriptor_keys);
    RUN_TEST(test_scan_particles_texture_contextual);
    RUN_TEST(test_scan_tilemap_descriptor_contextual);
    RUN_TEST(test_dependency_document_parses_required_optional_labels);
    RUN_TEST(test_dependency_document_rejects_wrong_contract);
    RUN_TEST(test_dependency_document_rejects_invalid_entries);
    RUN_TEST(test_scan_shader_graph_blobs);
    RUN_TEST(test_scan_backend_expansion_is_only_for_graph_blobs);
    return UNITY_END();
}
