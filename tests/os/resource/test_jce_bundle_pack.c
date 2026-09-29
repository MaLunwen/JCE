/* test_jce_bundle_pack.c
 *
 * Unit tests for the in-process scene bundle packer.
 */

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "unity.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_bundle_loader.h>
#include <jce/resource/jce_bundle_pack.h>
#include <jce/resource/jce_pak_loader.h>

#include <cjson/cJSON.h>
#include <xxhash.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char g_root[1024];
static int  g_error_count;
static int  g_warning_count;
static char g_last_error[512];

void setUp(void)
{
    static int counter;
    char base[1024];

    (void)jce_fs_host_get_current_dir(base, sizeof(base));
    snprintf(g_root, sizeof(g_root),
             "%s/_ut_bundle_pack_%d_%d", base,
             (int)(uintptr_t)setUp & 0xFFFF, ++counter);
    (void)jce_fs_host_remove_recursive(g_root);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_root));
    g_error_count = 0;
    g_warning_count = 0;
    g_last_error[0] = '\0';
}

void tearDown(void)
{
    (void)jce_fs_host_remove_recursive(g_root);
    /* Never leak a process-wide decryption key into the next test. */
    jce_archive_set_process_key(NULL);
}

static void join_path(char *out, size_t cap, const char *leaf)
{
    snprintf(out, cap, "%s/%s", g_root, leaf);
}

static void log_sink(JceBundlePackLogLevel level, const char *msg, void *user)
{
    (void)user;
    if (level == JCE_BUNDLE_PACK_LOG_ERROR) {
        ++g_error_count;
        snprintf(g_last_error, sizeof(g_last_error), "%s", msg ? msg : "");
    }
    if (level == JCE_BUNDLE_PACK_LOG_WARNING)
        ++g_warning_count;
}

static void init_single_file_opts(JceBundlePackOptions *opts,
                                  const char *scene_path,
                                  const char *out_dir,
                                  const char *const *scene_files)
{
    memset(opts, 0, sizeof(*opts));
    opts->scenes_dir       = g_root;
    opts->resource_root    = g_root;
    opts->out_dir          = out_dir;
    opts->scene_files      = scene_files;
    opts->scene_file_count = 1;
    opts->single_file_mode = true;
    opts->zstd_level       = 1;
    (void)scene_path;
}

static const cJSON *find_asset_record(const cJSON *assets, const char *key,
                                      const char *address)
{
    const cJSON *asset = NULL;
    cJSON_ArrayForEach(asset, assets) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(asset, key);
        if (cJSON_IsString(value) && value->valuestring &&
            strcmp(value->valuestring, address) == 0)
            return asset;
    }
    return NULL;
}

static const cJSON *find_dependency_record(const cJSON *dependencies,
                                           const char *address)
{
    return find_asset_record(dependencies, "address", address);
}

/* ── Always Included Shaders reach the bundle ─────────────────────────
 *
 * Unity's setting exists because a shader named only from a SCRIPT is
 * reachable from no material and gets stripped.  The same is true here of a
 * graph-compiled .bin blob or any asset a script names by string: no scene,
 * material or descriptor points at it, so the packer's closure never sees it
 * and the shipped game asks for a file that was never packed.
 *
 * The failure mode is the reason for the ERROR case below: nothing else
 * points at these assets, so nothing else would ever have caught a typo --
 * it would surface at runtime, on a player's machine, as a missing shader.
 */
static void test_always_included_reaches_the_bundle(void)
{
    char scene_path[1024], out_dir[1024], extra_path[1024], graph_path[1024];
    const char *scene_files[1];
    const char *scene_json = "{ \"entities\": [] }";
    const char *extra_blob = "not a real shader, but a real file";

    join_path(scene_path, sizeof(scene_path), "ai_empty.scene");
    join_path(out_dir,    sizeof(out_dir),    "out_ai");
    join_path(extra_path, sizeof(extra_path), "unreferenced_shader.bin");
    join_path(graph_path, sizeof(graph_path), "out_ai/bundle_graph.json");

    TEST_ASSERT_TRUE(jce_fs_host_create_directory(out_dir));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(scene_path, scene_json,
                                           (uint64_t)strlen(scene_json)));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(extra_path, extra_blob,
                                           (uint64_t)strlen(extra_blob)));
    scene_files[0] = scene_path;

    /* WITHOUT the list: the scene references nothing, so the blob does not
     * travel.  This half is the control -- without it, a bundle that happened
     * to sweep the directory would pass the assertion below for free. */
    {
        JceBundlePackOptions opts;
        init_single_file_opts(&opts, scene_path, out_dir, scene_files);
        g_error_count = 0;
        TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
        TEST_ASSERT_EQUAL_INT(0, g_error_count);

        uint64_t sz = 0;
        void *data = jce_fs_host_read_all(graph_path, &sz);
        TEST_ASSERT_NOT_NULL(data);
        TEST_ASSERT_NULL(strstr((const char *)data, "unreferenced_shader.bin"));
        jce_fs_buffer_free(data);
    }

    /* WITH the list: it travels. */
    {
        const char *always[1] = { "unreferenced_shader.bin" };
        JceBundlePackOptions opts;
        init_single_file_opts(&opts, scene_path, out_dir, scene_files);
        opts.always_included       = always;
        opts.always_included_count = 1;
        g_error_count = 0;
        TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
        TEST_ASSERT_EQUAL_INT(0, g_error_count);

        uint64_t sz = 0;
        void *data = jce_fs_host_read_all(graph_path, &sz);
        TEST_ASSERT_NOT_NULL(data);
        TEST_ASSERT_NOT_NULL(strstr((const char *)data,
                                    "unreferenced_shader.bin"));
        jce_fs_buffer_free(data);
    }

    /* A NAME THAT RESOLVES TO NOTHING FAILS THE RUN.  A warning would be the
     * wrong answer here specifically: the list is the only thing that names
     * these assets, so a silent skip is a build that is missing exactly what
     * somebody thought they had declared. */
    {
        const char *always[1] = { "typo_no_such_shader.bin" };
        JceBundlePackOptions opts;
        init_single_file_opts(&opts, scene_path, out_dir, scene_files);
        opts.always_included       = always;
        opts.always_included_count = 1;
        g_error_count = 0;
        TEST_ASSERT_NOT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
        TEST_ASSERT_TRUE(g_error_count > 0);
    }
}

static void test_single_file_scene_only_succeeds(void)
{
    char scene_path[1024];
    char out_dir[1024];
    char bundle_path[1024];
    char graph_path[1024];
    const char *scene_files[1];
    const char *scene_json = "{ \"entities\": [] }";

    join_path(scene_path, sizeof(scene_path), "empty.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(bundle_path, sizeof(bundle_path), "out/empty.jbundle");
    join_path(graph_path, sizeof(graph_path), "out/bundle_graph.json");

    TEST_ASSERT_TRUE(jce_fs_host_create_directory(out_dir));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(scene_path, scene_json,
                                          (uint64_t)strlen(scene_json)));

    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);

    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_error_count);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(bundle_path));
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(graph_path));

    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    JceBundleFile *bf = jce_bundle_file_open(fs, bundle_path, NULL);
    TEST_ASSERT_NOT_NULL(bf);
    TEST_ASSERT_EQUAL_STRING("empty.scene", jce_bundle_file_scene_path(bf));
    jce_bundle_file_close(bf);
    jce_fs_destroy(fs);

    char sidecar_path[1024];
    join_path(sidecar_path, sizeof(sidecar_path), "out/empty.jbundle.json");
    uint64_t bundle_size = 0;
    void *bundle_data = jce_fs_host_read_all(bundle_path, &bundle_size);
    TEST_ASSERT_NOT_NULL(bundle_data);
    uint64_t actual_hash = XXH3_64bits(bundle_data, (size_t)bundle_size);
    jce_fs_buffer_free(bundle_data);

    uint64_t sidecar_size = 0;
    void *sidecar_data = jce_fs_host_read_all(sidecar_path, &sidecar_size);
    TEST_ASSERT_NOT_NULL(sidecar_data);
    cJSON *sidecar = cJSON_ParseWithLength(
        (const char *)sidecar_data, (size_t)sidecar_size);
    TEST_ASSERT_NOT_NULL(sidecar);

    const cJSON *hash = cJSON_GetObjectItemCaseSensitive(
        sidecar, "content_hash");
    const cJSON *build_hash = cJSON_GetObjectItemCaseSensitive(
        sidecar, "build_hash");
    const cJSON *contract = cJSON_GetObjectItemCaseSensitive(
        sidecar, "contract");
    const cJSON *minor = contract ? cJSON_GetObjectItemCaseSensitive(
        contract, "minor") : NULL;
    TEST_ASSERT_TRUE(cJSON_IsString(hash));
    TEST_ASSERT_TRUE(cJSON_IsString(build_hash));
    TEST_ASSERT_TRUE(cJSON_IsNumber(minor));
    TEST_ASSERT_TRUE(minor->valueint >= 1);

    char actual_hex[17];
    snprintf(actual_hex, sizeof(actual_hex), "%016llx",
             (unsigned long long)actual_hash);
    TEST_ASSERT_EQUAL_STRING(actual_hex, hash->valuestring);

    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(sidecar, "assets");
    const cJSON *asset = cJSON_GetArrayItem(assets, 0);
    TEST_ASSERT_TRUE(cJSON_IsString(
        cJSON_GetObjectItemCaseSensitive(asset, "asset_id")));
    TEST_ASSERT_TRUE(cJSON_IsString(
        cJSON_GetObjectItemCaseSensitive(asset, "content_id")));
    TEST_ASSERT_TRUE(cJSON_IsString(
        cJSON_GetObjectItemCaseSensitive(asset, "type")));
    TEST_ASSERT_TRUE(cJSON_IsString(
        cJSON_GetObjectItemCaseSensitive(asset, "representation")));

    cJSON_Delete(sidecar);
    jce_fs_buffer_free(sidecar_data);

    uint64_t graph_size = 0;
    void *graph_data = jce_fs_host_read_all(graph_path, &graph_size);
    TEST_ASSERT_NOT_NULL(graph_data);
    cJSON *graph = cJSON_ParseWithLength(
        (const char *)graph_data, (size_t)graph_size);
    TEST_ASSERT_NOT_NULL(graph);
    const cJSON *graph_contract = cJSON_GetObjectItemCaseSensitive(
        graph, "contract");
    TEST_ASSERT_EQUAL_STRING("jce.bundle.graph",
        cJSON_GetObjectItemCaseSensitive(graph_contract, "name")->valuestring);
    const cJSON *graph_assets = cJSON_GetObjectItemCaseSensitive(
        graph, "assets");
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(graph_assets));
    const cJSON *graph_asset = cJSON_GetArrayItem(graph_assets, 0);
    TEST_ASSERT_EQUAL_STRING("empty.scene",
        cJSON_GetObjectItemCaseSensitive(graph_asset, "address")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsString(
        cJSON_GetObjectItemCaseSensitive(graph_asset, "content_id")));
    cJSON_Delete(graph);
    jce_fs_buffer_free(graph_data);
}

static void test_single_file_absolute_asset_is_virtualised(void)
{
    char scene_path[1024];
    char out_dir[1024];
    char asset_path[1024];
    char sidecar_path[1024];
    char scene_json[3072];
    const char *scene_files[1];

    join_path(scene_path, sizeof(scene_path), "external.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(asset_path, sizeof(asset_path), "outside_asset.bin");
    join_path(sidecar_path, sizeof(sidecar_path),
              "out/external.jbundle.json");
    for (char *p = asset_path; *p; ++p) {
        if (*p == '\\')
            *p = '/';
    }

    TEST_ASSERT_TRUE(jce_fs_host_create_directory(out_dir));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(asset_path, "asset", 5));
    snprintf(scene_json, sizeof(scene_json),
             "{ \"entities\": [ { \"meshPath\": \"%s\" } ] }", asset_path);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(scene_path, scene_json,
                                          (uint64_t)strlen(scene_json)));

    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);

    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_error_count);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(sidecar_path));

    uint64_t n = 0;
    void *buf = jce_fs_host_read_all(sidecar_path, &n);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr((const char *)buf, "_external/"));
    TEST_ASSERT_NULL(strstr((const char *)buf, asset_path));
    jce_fs_buffer_free(buf);
}

static void test_single_file_obj_missing_mtl_warns_but_succeeds(void)
{
    char scene_path[1024];
    char out_dir[1024];
    char obj_path[1024];
    char bundle_path[1024];
    char scene_json[3072];
    const char *scene_files[1];
    const char *obj_text =
        "# test obj\n"
        "mtllib missing_material.mtl\n"
        "o tri\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n";

    join_path(scene_path, sizeof(scene_path), "obj_missing_mtl.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(obj_path, sizeof(obj_path), "standalone.obj");
    join_path(bundle_path, sizeof(bundle_path),
              "out/obj_missing_mtl.jbundle");
    for (char *p = obj_path; *p; ++p) {
        if (*p == '\\')
            *p = '/';
    }

    TEST_ASSERT_TRUE(jce_fs_host_create_directory(out_dir));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(obj_path, obj_text,
                                          (uint64_t)strlen(obj_text)));
    snprintf(scene_json, sizeof(scene_json),
             "{ \"entities\": [ { \"meshPath\": \"%s\" } ] }", obj_path);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(scene_path, scene_json,
                                          (uint64_t)strlen(scene_json)));

    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);

    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_error_count);
    TEST_ASSERT_TRUE(g_warning_count > 0);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(bundle_path));
}

static void test_single_file_missing_asset_fails_without_half_bundle(void)
{
    char scene_path[1024];
    char out_dir[1024];
    char bundle_path[1024];
    char sidecar_path[1024];
    const char *scene_files[1];
    const char *scene_json =
        "{"
        "  \"entities\": ["
        "    { \"mesh\": { \"meshPath\": \"models/missing.obj\" } }"
        "  ]"
        "}";

    join_path(scene_path, sizeof(scene_path), "showcase.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(bundle_path, sizeof(bundle_path), "out/showcase.jbundle");
    join_path(sidecar_path, sizeof(sidecar_path), "out/showcase.jbundle.json");

    TEST_ASSERT_TRUE(jce_fs_host_create_directory(out_dir));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(scene_path, scene_json,
                                          (uint64_t)strlen(scene_json)));

    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);

    TEST_ASSERT_NOT_EQUAL(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_TRUE(g_error_count > 0);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(bundle_path));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(sidecar_path));
}

/* ── Encrypted bundle: pack → mount with the process key → read ───────── */

static void test_single_file_encrypted_pack_mount_read(void)
{
    static const uint8_t key[32] = {
        0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
        0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
        0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,
        0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    };

    char scene_path[1024];
    char out_dir[1024];
    char bundle_path[1024];
    const char *scene_files[1];
    const char *scene_json = "{ \"entities\": [] }";

    join_path(scene_path, sizeof(scene_path), "locked.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(bundle_path, sizeof(bundle_path), "out/locked.jbundle");

    TEST_ASSERT_TRUE(jce_fs_host_create_directory(out_dir));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(scene_path, scene_json,
                                          (uint64_t)strlen(scene_json)));

    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);
    opts.encrypt        = true;
    opts.encryption_key = key;

    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_error_count);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(bundle_path));

    /* Secure bundles expose neither paths nor payloads without the key. */
    {
        JceArchive *archive = jce_archive_open_file(bundle_path);
        TEST_ASSERT_NOT_NULL(archive);
        TEST_ASSERT_TRUE(jce_archive_is_secure(archive));
        TEST_ASSERT_TRUE(jce_archive_is_authenticated(archive));
        TEST_ASSERT_EQUAL_INT(JCE_ARCHIVE_AUTH_UNAVAILABLE,
                              jce_archive_auth_status(archive));
        for (uint32_t i = 0; i < jce_archive_count(archive); ++i)
            TEST_ASSERT_NULL(jce_archive_debug_path(archive, i));
        TEST_ASSERT_NULL(jce_archive_find(archive, "locked.scene"));
        jce_archive_close(archive);

        JcePakArchive *pak = jce_pak_open_file(bundle_path);
        TEST_ASSERT_NOT_NULL(pak);
        const JcePakAsset *a = jce_pak_find(pak, "locked.scene");
        TEST_ASSERT_NULL(a);
        jce_pak_close(pak);
    }

    /* With the process key installed (the runtime boot path), the bundle
     * opens and the scene reads back as authored. */
    jce_archive_set_process_key(key);
    {
        JceArchive *archive = jce_archive_open_file(bundle_path);
        TEST_ASSERT_NOT_NULL(archive);
        TEST_ASSERT_EQUAL_INT(JCE_ARCHIVE_AUTH_VALID,
                              jce_archive_auth_status(archive));
        TEST_ASSERT_NOT_NULL(jce_archive_find(archive, "locked.scene"));
        jce_archive_close(archive);

        JceFileSystem *fs = jce_fs_create();
        TEST_ASSERT_NOT_NULL(fs);
        JceBundleFile *bf = jce_bundle_file_open(fs, bundle_path, NULL);
        TEST_ASSERT_NOT_NULL(bf);
        TEST_ASSERT_EQUAL_STRING("locked.scene",
                                 jce_bundle_file_scene_path(bf));
        jce_bundle_file_close(bf);
        jce_fs_destroy(fs);

        JcePakArchive *pak = jce_pak_open_file(bundle_path);
        TEST_ASSERT_NOT_NULL(pak);
        const JcePakAsset *a = jce_pak_find(pak, "locked.scene");
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQUAL_UINT64((uint64_t)strlen(scene_json),
                                 a->original_size);
        char tmp[256];
        TEST_ASSERT_EQUAL_size_t(strlen(scene_json),
            jce_pak_decompress(a, tmp, sizeof(tmp)));
        TEST_ASSERT_EQUAL_MEMORY(scene_json, tmp, strlen(scene_json));
        jce_pak_close(pak);
    }
    jce_archive_set_process_key(NULL);
}

/* ── Completeness: nested descriptors + convention sidecars + i18n ────── */

static void write_text(const char *leaf, const char *text)
{
    char p[1024];
    join_path(p, sizeof(p), leaf);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(p, text, (uint64_t)strlen(text)));
}

static void make_dir(const char *leaf)
{
    char p[1024];
    join_path(p, sizeof(p), leaf);
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(p));
}

static void test_single_file_nested_descriptors_and_sidecars(void)
{
    char scene_path[1024];
    char out_dir[1024];
    char sidecar_path[1024];
    const char *scene_files[1];

    make_dir("mats");
    make_dir("fx");
    make_dir("maps");
    make_dir("models");
    make_dir("anim");
    make_dir("tex");
    make_dir("i18n");
    make_dir("out");

    /* Scene: material + particles + tilemap + skeletal animator (+SM). */
    write_text("demo.scene.json",
        "{ \"entities\": [ { \"components\": ["
        "  { \"type\": \"MeshRenderer\", \"materialPath\": \"mats/m.mat.json\" },"
        "  { \"type\": \"ParticleEmitter\", \"assetPath\": \"fx/p.particles.json\" },"
        "  { \"type\": \"Terrain\","
        "    \"layerNormalPath0\": \"tex/terrain_n.png\","
        "    \"layerMaskPath0\": \"tex/terrain_mask.png\" },"
        "  { \"type\": \"Tilemap\", \"properties\": {"
        "      \"tilemapPath\": \"maps/t.tilemap.json\" } },"
        "  { \"type\": \"SkeletalAnimator\","
        "    \"skeletonPath\": \"models/hero.glb\","
        "    \"stateMachine\": \"anim/loco.anim_sm.json\" }"
        "] } ] }");

    /* Material: one descriptor-relative ref, one root-relative ref. */
    write_text("mats/m.mat.json",
        "{ \"properties\": {"
        "   \"albedoMap\": \"../tex/a.png\","
        "   \"normalMap\": \"tex/n.png\""
        "} }");
    write_text("tex/a.png", "PNGa");
    write_text("tex/n.png", "PNGn");
    write_text("tex/terrain_n.png", "PNGtn");
    write_text("tex/terrain_mask.png", "PNGtm");

    /* Particle descriptor: contextual \"texture\" key. */
    write_text("fx/p.particles.json",
        "{ \"emitRate\": 24, \"lifetimeMin\": 0.5,"
        "  \"texture\": \"tex/spark.png\" }");
    write_text("tex/spark.png", "PNGs");

    /* Tilemap → tileset → atlas image chain. */
    write_text("maps/t.tilemap.json",
        "{ \"w\": 2, \"h\": 1, \"cells\": [1, 0],"
        "  \"sprites\": \"maps/ts.sprites.json\" }");
    write_text("maps/ts.sprites.json",
        "{ \"source\": \"tex/tiles.png\", \"sourceW\": 32, \"sourceH\": 32,"
        "  \"rects\": [ { \"x\": 0, \"y\": 0, \"w\": 16, \"h\": 16 } ] }");
    write_text("tex/tiles.png", "PNGt");

    /* Skeleton + convention sidecars + anim state machine. */
    write_text("models/hero.glb", "GLB0");
    write_text("models/hero.glb.anim.json",
        "{ \"Run\": [ { \"time\": 0.25, \"name\": \"footstep\" } ] }");
    write_text("models/hero.glb.jcol", "JCOL");
    write_text("anim/loco.anim_sm.json",
        "{ \"params\": [ { \"name\": \"Speed\", \"type\": \"float\" } ],"
        "  \"states\": [ { \"name\": \"Idle\", \"clip\": \"Idle\" } ] }");

    /* Scene navmesh sidecar (same basename) + game L10n tables. */
    write_text("demo.navmesh.bin", "NAVM");
    write_text("i18n/en.json",    "{ \"hello\": \"Hello\" }");
    write_text("i18n/zh_cn.json", "{ \"hello\": \"\\u4f60\\u597d\" }");

    join_path(scene_path, sizeof(scene_path), "demo.scene.json");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(sidecar_path, sizeof(sidecar_path), "out/demo.jbundle.json");
    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);

    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_error_count);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(sidecar_path));

    uint64_t n = 0;
    void *buf = jce_fs_host_read_all(sidecar_path, &n);
    TEST_ASSERT_NOT_NULL(buf);
    const char *m = (const char *)buf;

    /* Direct component refs. */
    TEST_ASSERT_NOT_NULL(strstr(m, "mats/m.mat.json"));
    TEST_ASSERT_NOT_NULL(strstr(m, "fx/p.particles.json"));
    TEST_ASSERT_NOT_NULL(strstr(m, "maps/t.tilemap.json"));
    TEST_ASSERT_NOT_NULL(strstr(m, "models/hero.glb"));
    TEST_ASSERT_NOT_NULL(strstr(m, "anim/loco.anim_sm.json"));
    /* Manifests expose only the canonical address used by runtime lookup. */
    TEST_ASSERT_NOT_NULL(strstr(m, "tex/a.png"));
    TEST_ASSERT_NOT_NULL(strstr(m, "tex/n.png"));
    TEST_ASSERT_NOT_NULL(strstr(m, "tex/terrain_n.png"));
    TEST_ASSERT_NOT_NULL(strstr(m, "tex/terrain_mask.png"));
    /* Nested: particles texture, tileset chain. */
    TEST_ASSERT_NOT_NULL(strstr(m, "tex/spark.png"));
    TEST_ASSERT_NOT_NULL(strstr(m, "maps/ts.sprites.json"));
    TEST_ASSERT_NOT_NULL(strstr(m, "tex/tiles.png"));
    /* Convention sidecars. */
    TEST_ASSERT_NOT_NULL(strstr(m, "models/hero.glb.anim.json"));
    TEST_ASSERT_NOT_NULL(strstr(m, "models/hero.glb.jcol"));
    TEST_ASSERT_NOT_NULL(strstr(m, "demo.navmesh.bin"));
    /* Localization tables. */
    TEST_ASSERT_NOT_NULL(strstr(m, "i18n/en.json"));
    TEST_ASSERT_NOT_NULL(strstr(m, "i18n/zh_cn.json"));

    jce_fs_buffer_free(buf);
}

static void test_single_file_explicit_dependencies_and_project_roots(void)
{
    char scene_path[1024];
    char out_dir[1024];
    char manifest_path[1024];
    char graph_path[1024];
    const char *scene_files[1];

    make_dir("scripts");
    make_dir("audio");
    make_dir("config");
    make_dir("out");
    write_text("deps.scene",
        "{ \"entities\": [ { \"scriptPath\": \"scripts/director.lua\" } ] }");
    write_text("scripts/director.lua", "return {}\n");
    write_text("audio/thunder.mp3", "MP3");
    write_text("config/runtime.json", "{ \"quality\": \"test\" }");
    write_text("scripts/director.lua.deps.json",
        "{"
        " \"contract\": { \"name\": \"jce.bundle.dependencies\","
        "   \"major\": 1, \"minor\": 0 },"
        " \"assets\": [\"audio/thunder.mp3\"],"
        " \"optional_assets\": [\"audio/not-installed.ogg\"],"
        " \"labels\": [\"runtime\"]"
        "}");
    write_text("bundle_roots.json",
        "{"
        " \"contract\": { \"name\": \"jce.bundle.dependencies\","
        "   \"major\": 1, \"minor\": 0 },"
        " \"assets\": ["
        "   { \"path\": \"config/runtime.json\","
        "     \"bundle\": \"force-shared\" }"
        " ]"
        "}");

    join_path(scene_path, sizeof(scene_path), "deps.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(manifest_path, sizeof(manifest_path), "out/deps.jbundle.json");
    join_path(graph_path, sizeof(graph_path), "out/bundle_graph.json");
    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);

    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_EQUAL_INT(0, g_error_count);
    TEST_ASSERT_TRUE(g_warning_count > 0);

    uint64_t n = 0;
    void *buf = jce_fs_host_read_all(manifest_path, &n);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_NOT_NULL(strstr((const char *)buf, "audio/thunder.mp3"));
    TEST_ASSERT_NOT_NULL(strstr((const char *)buf, "config/runtime.json"));
    TEST_ASSERT_NULL(strstr((const char *)buf, ".deps.json"));
    TEST_ASSERT_NULL(strstr((const char *)buf, "audio/not-installed.ogg"));

    cJSON *manifest = cJSON_ParseWithLength((const char *)buf, (size_t)n);
    TEST_ASSERT_NOT_NULL(manifest);
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(manifest, "assets");
    const cJSON *scene = find_asset_record(assets, "path", "deps.scene");
    const cJSON *script = find_asset_record(
        assets, "path", "scripts/director.lua");
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(script);

    const cJSON *scene_deps = cJSON_GetObjectItemCaseSensitive(
        scene, "dependencies");
    const cJSON *script_dep = find_dependency_record(
        scene_deps, "scripts/director.lua");
    const cJSON *root_dep = find_dependency_record(
        scene_deps, "config/runtime.json");
    TEST_ASSERT_NOT_NULL(script_dep);
    TEST_ASSERT_NOT_NULL(root_dep);
    TEST_ASSERT_EQUAL_STRING("scene",
        cJSON_GetObjectItemCaseSensitive(script_dep, "origin")->valuestring);
    TEST_ASSERT_EQUAL_STRING("project_root",
        cJSON_GetObjectItemCaseSensitive(root_dep, "origin")->valuestring);

    const cJSON *script_deps = cJSON_GetObjectItemCaseSensitive(
        script, "dependencies");
    const cJSON *audio_dep = find_dependency_record(
        script_deps, "audio/thunder.mp3");
    TEST_ASSERT_NOT_NULL(audio_dep);
    TEST_ASSERT_EQUAL_STRING("dependency_document",
        cJSON_GetObjectItemCaseSensitive(audio_dep, "origin")->valuestring);
    cJSON_Delete(manifest);
    jce_fs_buffer_free(buf);

    uint64_t graph_size = 0;
    void *graph_buf = jce_fs_host_read_all(graph_path, &graph_size);
    TEST_ASSERT_NOT_NULL(graph_buf);
    cJSON *graph = cJSON_ParseWithLength(
        (const char *)graph_buf, (size_t)graph_size);
    TEST_ASSERT_NOT_NULL(graph);
    const cJSON *graph_assets = cJSON_GetObjectItemCaseSensitive(
        graph, "assets");
    const cJSON *graph_script = find_asset_record(
        graph_assets, "address", "scripts/director.lua");
    TEST_ASSERT_NOT_NULL(graph_script);
    TEST_ASSERT_NOT_NULL(find_dependency_record(
        cJSON_GetObjectItemCaseSensitive(graph_script, "dependencies"),
        "audio/thunder.mp3"));
    cJSON_Delete(graph);
    jce_fs_buffer_free(graph_buf);
}

static void test_single_file_malformed_dependency_document_fails(void)
{
    char scene_path[1024];
    char out_dir[1024];
    char bundle_path[1024];
    const char *scene_files[1];

    make_dir("scripts");
    make_dir("out");
    write_text("invalid_deps.scene",
        "{ \"entities\": [ { \"scriptPath\": \"scripts/bad.lua\" } ] }");
    write_text("scripts/bad.lua", "return {}\n");
    write_text("scripts/bad.lua.deps.json",
        "{ \"contract\": { \"name\": \"jce.bundle\","
        "  \"major\": 1, \"minor\": 0 }, \"assets\": [] }");

    join_path(scene_path, sizeof(scene_path), "invalid_deps.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(bundle_path, sizeof(bundle_path), "out/invalid_deps.jbundle");
    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);

    TEST_ASSERT_NOT_EQUAL(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_TRUE(g_error_count > 0);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(bundle_path));
}

static void test_single_file_missing_explicit_dependency_fails(void)
{
    char scene_path[1024];
    char out_dir[1024];
    char bundle_path[1024];
    const char *scene_files[1];

    make_dir("scripts");
    make_dir("out");
    write_text("missing_deps.scene",
        "{ \"entities\": [ { \"scriptPath\": \"scripts/missing.lua\" } ] }");
    write_text("scripts/missing.lua", "return {}\n");
    write_text("scripts/missing.lua.deps.json",
        "{"
        " \"contract\": { \"name\": \"jce.bundle.dependencies\","
        "   \"major\": 1, \"minor\": 0 },"
        " \"assets\": [\"audio/required-but-missing.ogg\"]"
        "}");

    join_path(scene_path, sizeof(scene_path), "missing_deps.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(bundle_path, sizeof(bundle_path), "out/missing_deps.jbundle");
    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);

    TEST_ASSERT_NOT_EQUAL(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_TRUE(g_error_count > 0);
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(bundle_path));
    TEST_ASSERT_NOT_NULL(strstr(g_last_error,
        "missing_deps.scene --scene--> scripts/missing.lua"
        " --dependency_document--> audio/required-but-missing.ogg"));
}

static void test_single_file_rejects_canonical_address_collision(void)
{
    char scene_path[1024];
    char out_dir[1024];
    char bundle_path[1024];
    const char *scene_files[1];

    make_dir("tex");
    make_dir("out");
    write_text("tex/a.png", "PNG");
    write_text("collision.scene",
        "{ \"entities\": ["
        " { \"texturePath\": \"tex/a.png\" },"
        " { \"texturePath\": \"tex/../tex/a.png\" }"
        "] }");

    join_path(scene_path, sizeof(scene_path), "collision.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(bundle_path, sizeof(bundle_path), "out/collision.jbundle");
    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);

    TEST_ASSERT_NOT_EQUAL(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_NOT_NULL(strstr(g_last_error, "canonical address collision"));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(bundle_path));
}

/* 1x1 opaque red PNG, RGBA8, all chunk CRCs valid.  Small enough to inline,
 * real enough that the texture cooker actually decodes and re-encodes it. */
static const uint8_t k_red_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
    0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
    0x54, 0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x72, 0x9c,
    0x52, 0x67, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

static void write_bytes(const char *leaf, const void *data, size_t n)
{
    char p[1024];
    join_path(p, sizeof(p), leaf);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(p, data, (uint64_t)n));
}

/* Cover the PARALLEL cook phase (>= 2 cookable assets), which nothing else in
 * this file reaches — every other test packs without cook_assets, and the
 * JCE_PACK_COOK harness below is env-gated and skipped in CI.
 *
 * That phase runs on a pool the packer creates for itself.  It used to borrow
 * jce_thread_pool_shared(), which was wrong twice over: the editor calls
 * jce_bundle_pack_run on its own build thread, and an unregistered thread's
 * enkiTS submissions land in thread slot 0 — the frame loop's single-producer
 * pipe.  So this test's real job is to keep the parallel path exercised at all,
 * on every run, rather than only when someone sets an environment variable. */
static void test_parallel_cook_of_multiple_textures(void)
{
    char scene_path[1024];
    char out_dir[1024];
    char bundle_path[1024];
    char manifest_path[1024];
    const char *scene_files[1];

    make_dir("scripts");
    make_dir("textures");
    make_dir("out");

    write_text("cook2.scene",
        "{ \"entities\": [ { \"scriptPath\": \"scripts/looks.lua\" } ] }");
    write_text("scripts/looks.lua", "return {}\n");
    write_bytes("textures/a.png", k_red_png, sizeof(k_red_png));
    write_bytes("textures/b.png", k_red_png, sizeof(k_red_png));
    write_text("scripts/looks.lua.deps.json",
        "{"
        " \"contract\": { \"name\": \"jce.bundle.dependencies\","
        "   \"major\": 1, \"minor\": 0 },"
        " \"assets\": [\"textures/a.png\", \"textures/b.png\"]"
        "}");

    join_path(scene_path, sizeof(scene_path), "cook2.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(bundle_path, sizeof(bundle_path), "out/cook2.jbundle");
    join_path(manifest_path, sizeof(manifest_path), "out/cook2.jbundle.json");
    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);
    opts.cook_assets     = true;
    opts.target_platform = 0;      /* JCE_COOK_PLATFORM_WINDOWS */

    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_error_count, g_last_error);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(bundle_path));

    /* Both textures survived the parallel phase — a cook job that silently
     * dropped its output would still leave a valid bundle behind. */
    uint64_t n = 0;
    void *buf = jce_fs_host_read_all(manifest_path, &n);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_NOT_NULL(strstr((const char *)buf, "textures/a.png"));
    TEST_ASSERT_NOT_NULL(strstr((const char *)buf, "textures/b.png"));
    jce_fs_buffer_free(buf);
}

/* ── Manual harness: pack real scenes named via environment ───────────────
 * Set JCE_PACK_SCENES (';'-separated scene paths), JCE_PACK_ROOT (resource
 * root) and JCE_PACK_OUT (output dir) to run a real pack through the test
 * binary.  Ignored (skipped) when the variables are absent, so CI is
 * unaffected. */
static void test_pack_env_scenes(void)
{
    const char *scenes = getenv("JCE_PACK_SCENES");
    const char *root   = getenv("JCE_PACK_ROOT");
    const char *out    = getenv("JCE_PACK_OUT");
    if (!scenes || !root || !out) {
        TEST_IGNORE_MESSAGE("JCE_PACK_SCENES/JCE_PACK_ROOT/JCE_PACK_OUT unset");
        return;
    }

    static char buf[4096];
    snprintf(buf, sizeof(buf), "%s", scenes);
    const char *files[16];
    size_t nf = 0;
    char *tok = buf;
    while (tok && *tok && nf < 16) {
        char *semi = strchr(tok, ';');
        if (semi) *semi = '\0';
        if (*tok) files[nf++] = tok;
        tok = semi ? semi + 1 : NULL;
    }
    TEST_ASSERT_TRUE(nf > 0);

    JceBundlePackOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.scenes_dir       = root;
    opts.resource_root    = root;
    opts.out_dir          = out;
    opts.scene_files      = files;
    opts.scene_file_count = nf;
    opts.zstd_level       = 1;
    /* Opt in to the full cook pipeline (texture BC/ASTC + meshopt-GLB) so
     * this harness exercises the real build path; JCE_PACK_COOK gates it. */
    opts.cook_assets      = getenv("JCE_PACK_COOK") != NULL;
    opts.target_platform  = 0; /* JCE_COOK_PLATFORM_WINDOWS */

    struct timespec t0, t1;
    timespec_get(&t0, TIME_UTC);
    int rc = jce_bundle_pack_run(&opts, log_sink, NULL);
    timespec_get(&t1, TIME_UTC);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("[env-pack] rc=%d errors=%d warnings=%d scenes=%zu cook=%d time=%.2fs\n",
           rc, g_error_count, g_warning_count, nf,
           (int)opts.cook_assets, secs);
}

/* A cooked .jceasset texture (block-compressed) must decode back to RGBA8
 * via the engine helper the editor uses to preview bundle/PAK textures.
 * Point JCE_COOKED_TEX at a .jceasset produced by `jce_cook ... --texfmt bc3
 * --mipmaps`. */
static void test_cooked_texture_decodes_to_rgba(void)
{
    const char *p = getenv("JCE_COOKED_TEX");
    if (!p) {
        TEST_IGNORE_MESSAGE("JCE_COOKED_TEX unset");
        return;
    }
    uint64_t n = 0;
    void *buf = jce_fs_host_read_all(p, &n);
    TEST_ASSERT_NOT_NULL(buf);

    uint8_t *rgba = NULL;
    uint32_t w = 0, h = 0;
    bool ok = jce_texture_decode_cooked_rgba8(buf, (size_t)n, &rgba, &w, &h);
    printf("[cooked-decode] ok=%d %ux%u\n", (int)ok, w, h);
    jce_fs_buffer_free(buf);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(w > 0 && h > 0);
    TEST_ASSERT_NOT_NULL(rgba);
    /* Not all-zero (a decode that silently produced a blank buffer). */
    int nonzero = 0;
    for (uint32_t i = 0; i < w * h * 4u && !nonzero; ++i)
        if (rgba[i]) nonzero = 1;
    TEST_ASSERT_TRUE(nonzero);
    jce_free(rgba);
}

/* ── Every script language reaches the SHIPPED archive ────────────────── *
 *
 * The failure this exists for is invisible in the editor: it keeps running a
 * .py from loose files while the packaged build ships without it, or ships it
 * labelled "binary" so no tooling can tell it is code.  Both halves are
 * asserted, and the archive half matters most — a language can be
 * cookable-in-theory (manifest says "script") and still absent from the pak.
 */
static void test_every_script_language_reaches_the_packaged_bundle(void)
{
    /* `address` is `vpath` after jce_archive_normalize_path, which lowercases
     * — resource identity is case-insensitive.  The Java files are authored
     * with the capital their language requires, so this also proves the
     * extension match survives the case fold end to end. */
    static const struct {
        const char *vpath;
        const char *address;
        const char *body;
        const char *representation;
    } k_scripts[] = {
        { "scripts/bob.lua",      "scripts/bob.lua",
          "return {}\n",                  "lua.source"    },
        { "scripts/turret.py",    "scripts/turret.py",
          "def on_start(e):\n    pass\n", "python.source" },
        { "scripts/Turret.java",  "scripts/turret.java",
          "class Turret {}\n",            "java.source"   },
        { "scripts/Turret.class", "scripts/turret.class",
          "CAFEBABE-not-really\n",        "java.class"    },
    };
    const size_t k_count = sizeof(k_scripts) / sizeof(k_scripts[0]);

    char scene_path[1024];
    char out_dir[1024];
    char bundle_path[1024];
    char manifest_path[1024];
    const char *scene_files[1];

    make_dir("scripts");
    make_dir("out");
    write_text("langs.scene",
        "{ \"entities\": ["
        "  { \"scriptPath\": \"scripts/bob.lua\" },"
        "  { \"scriptPath\": \"scripts/turret.py\" },"
        "  { \"scriptPath\": \"scripts/Turret.java\" },"
        "  { \"scriptPath\": \"scripts/Turret.class\" }"
        "] }");
    for (size_t i = 0; i < k_count; ++i)
        write_text(k_scripts[i].vpath, k_scripts[i].body);

    join_path(scene_path, sizeof(scene_path), "langs.scene");
    join_path(out_dir, sizeof(out_dir), "out");
    join_path(bundle_path, sizeof(bundle_path), "out/langs.jbundle");
    join_path(manifest_path, sizeof(manifest_path), "out/langs.jbundle.json");
    scene_files[0] = scene_path;

    JceBundlePackOptions opts;
    init_single_file_opts(&opts, scene_path, out_dir, scene_files);

    TEST_ASSERT_EQUAL_INT(0, jce_bundle_pack_run(&opts, log_sink, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_error_count, g_last_error);

    /* 1. The manifest calls each one a script, in its own language. */
    uint64_t n = 0;
    void *buf = jce_fs_host_read_all(manifest_path, &n);
    TEST_ASSERT_NOT_NULL(buf);
    cJSON *manifest = cJSON_ParseWithLength((const char *)buf, (size_t)n);
    TEST_ASSERT_NOT_NULL(manifest);
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(manifest, "assets");
    for (size_t i = 0; i < k_count; ++i) {
        const cJSON *rec = find_asset_record(assets, "path",
                                             k_scripts[i].address);
        TEST_ASSERT_NOT_NULL_MESSAGE(rec, k_scripts[i].address);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("script",
            cJSON_GetObjectItemCaseSensitive(rec, "type")->valuestring,
            k_scripts[i].address);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(k_scripts[i].representation,
            cJSON_GetObjectItemCaseSensitive(rec, "representation")->valuestring,
            k_scripts[i].address);
    }
    cJSON_Delete(manifest);
    jce_fs_buffer_free(buf);

    /* 2. And the BYTES are really in the archive a shipped game opens.  The
     *    manifest is a description; this is the thing that runs. */
    JcePakArchive *pak = jce_pak_open_file(bundle_path);
    TEST_ASSERT_NOT_NULL(pak);
    for (size_t i = 0; i < k_count; ++i) {
        const JcePakAsset *a = jce_pak_find(pak, k_scripts[i].address);
        TEST_ASSERT_NOT_NULL_MESSAGE(a, k_scripts[i].address);
        const size_t want = strlen(k_scripts[i].body);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)want, a->original_size,
                                         k_scripts[i].address);
        char tmp[256];
        TEST_ASSERT_EQUAL_size_t_MESSAGE(want,
            jce_pak_decompress(a, tmp, sizeof(tmp)), k_scripts[i].address);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(k_scripts[i].body, tmp, want,
                                         k_scripts[i].address);
    }
    jce_pak_close(pak);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cooked_texture_decodes_to_rgba);
    RUN_TEST(test_single_file_scene_only_succeeds);
    RUN_TEST(test_single_file_absolute_asset_is_virtualised);
    RUN_TEST(test_single_file_obj_missing_mtl_warns_but_succeeds);
    RUN_TEST(test_single_file_missing_asset_fails_without_half_bundle);
    RUN_TEST(test_single_file_encrypted_pack_mount_read);
    RUN_TEST(test_single_file_nested_descriptors_and_sidecars);
    RUN_TEST(test_single_file_explicit_dependencies_and_project_roots);
    RUN_TEST(test_single_file_malformed_dependency_document_fails);
    RUN_TEST(test_single_file_missing_explicit_dependency_fails);
    RUN_TEST(test_single_file_rejects_canonical_address_collision);
    RUN_TEST(test_every_script_language_reaches_the_packaged_bundle);
    RUN_TEST(test_parallel_cook_of_multiple_textures);
    RUN_TEST(test_pack_env_scenes);
    RUN_TEST(test_always_included_reaches_the_bundle);
    return UNITY_END();
}
