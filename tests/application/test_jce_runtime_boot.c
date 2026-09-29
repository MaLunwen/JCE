/*
 * test_jce_runtime_boot.c
 *
 * Runtime boot metadata is part of the shipping PAK contract. The parser must
 * accept a valid virtual scene path and reject host-path escapes before the
 * default application attempts to load anything.
 */

#include <jce/application/jce_runtime_boot.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_sequencer.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_archive_cook.h>
#include <jce/resource/jce_image_decode.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/resource/jce_scene_serial.h>
#include <jce/renderer/jce_sprite.h>

#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const void *JCE_CALL test_pak_find(const JcePakArchive *pak,
                                          const char *path)
{
    return jce_pak_find(pak, path);
}

static uint64_t JCE_CALL test_pak_asset_size(const void *asset)
{
    return ((const JcePakAsset *)asset)->original_size;
}

static size_t JCE_CALL test_pak_decompress(const void *asset, void *out,
                                           size_t out_size)
{
    return jce_pak_decompress((const JcePakAsset *)asset, out, out_size);
}

static const JceFsPakProvider TEST_PAK_PROVIDER = {
    test_pak_find, test_pak_asset_size, test_pak_decompress
};

static void test_parse_accepts_relative_startup_scene(void)
{
    static const char json[] =
        "{"
        "\"contract\":\"jce.runtime_boot\","
        "\"schema\":1,"
        "\"startup_scene\":\"scenes/main.scene.json\""
        "}";
    JceRuntimeBootManifest boot;

    TEST_ASSERT_TRUE(jce_runtime_boot_manifest_parse(json, strlen(json),
                                                     &boot));
    TEST_ASSERT_EQUAL_STRING("scenes/main.scene.json", boot.startup_scene);
}

static void test_parse_rejects_wrong_contract(void)
{
    static const char json[] =
        "{"
        "\"contract\":\"jce.project\","
        "\"schema\":1,"
        "\"startup_scene\":\"scenes/main.scene.json\""
        "}";
    JceRuntimeBootManifest boot;

    TEST_ASSERT_FALSE(jce_runtime_boot_manifest_parse(json, strlen(json),
                                                      &boot));
}

static void test_parse_rejects_host_path_escape(void)
{
    static const char json[] =
        "{"
        "\"contract\":\"jce.runtime_boot\","
        "\"schema\":1,"
        "\"startup_scene\":\"../outside.scene.json\""
        "}";
    JceRuntimeBootManifest boot;

    TEST_ASSERT_FALSE(jce_runtime_boot_manifest_parse(json, strlen(json),
                                                      &boot));
}

static void test_parse_allows_explicit_empty_project(void)
{
    static const char json[] =
        "{"
        "\"contract\":\"jce.runtime_boot\","
        "\"schema\":1,"
        "\"startup_scene\":\"\""
        "}";
    JceRuntimeBootManifest boot;

    TEST_ASSERT_TRUE(jce_runtime_boot_manifest_parse(json, strlen(json),
                                                     &boot));
    TEST_ASSERT_EQUAL_STRING("", boot.startup_scene);
}

static void test_loads_manifest_from_pak(void)
{
    static const char json[] =
        "{"
        "\"contract\":\"jce.runtime_boot\","
        "\"schema\":1,"
        "\"startup_scene\":\"scenes/main.scene.json\""
        "}";
    const JceCookInput input = {
        JCE_RUNTIME_BOOT_MANIFEST_PATH, json, sizeof(json) - 1
    };
    JceCookConfig config = {0};
    JceRuntimeBootManifest boot;
    void *pak_blob = NULL;
    size_t pak_size = 0;
    JcePakArchive *pak;

    TEST_ASSERT_TRUE(jce_archive_cook(&input, 1, &config, &pak_blob,
                                      &pak_size, NULL));
    TEST_ASSERT_NOT_NULL(pak_blob);
    pak = jce_pak_open_owned(pak_blob, pak_size);
    TEST_ASSERT_NOT_NULL(pak);
    TEST_ASSERT_TRUE(jce_runtime_boot_manifest_load_pak(pak, &boot));
    TEST_ASSERT_EQUAL_STRING("scenes/main.scene.json", boot.startup_scene);
    jce_pak_close(pak);
}

static void test_decodes_image_from_pak(void)
{
    /* 1x1 opaque red, RGBA8 (IHDR colour type 6, bit depth 8).
     *
     * The IDAT CRC below (72 9c 52 67) must stay correct.  It used to be
     * 89 99 3d 1d — wrong — and the test still passed only because the
     * decoder behind jce_image_decode was stb_image, which does not verify
     * chunk CRCs.  Routing every decode through the one image service put
     * libpng underneath this call, and libpng does verify: the malformed
     * fixture failed with "IDAT: CRC error".  The fixture was the bug. */
    static const uint8_t png[] = {
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
    const JceCookInput input = {
        "textures/grass_mask.bin", png, sizeof(png)
    };
    JceCookConfig config = {0};
    void *pak_blob = NULL;
    size_t pak_size = 0;
    JceImage image = {0};

    TEST_ASSERT_TRUE(jce_archive_cook(&input, 1, &config, &pak_blob,
                                      &pak_size, NULL));
    JcePakArchive *pak = jce_pak_open_owned(pak_blob, pak_size);
    TEST_ASSERT_NOT_NULL(pak);
    TEST_ASSERT_TRUE(jce_image_decode_pak(pak, "textures/grass_mask.bin",
                                          &image));
    TEST_ASSERT_EQUAL_UINT32(1, image.width);
    TEST_ASSERT_EQUAL_UINT32(1, image.height);
    /* Pixel values, not just dimensions: this is the only place the PAK
     * decode path's OUTPUT is pinned, and PNG is lossless, so the byte
     * sequence is identical whichever codec the service picks. */
    TEST_ASSERT_NOT_NULL(image.pixels);
    TEST_ASSERT_EQUAL_UINT8(0xff, image.pixels[0]);   /* R */
    TEST_ASSERT_EQUAL_UINT8(0x00, image.pixels[1]);   /* G */
    TEST_ASSERT_EQUAL_UINT8(0x00, image.pixels[2]);   /* B */
    TEST_ASSERT_EQUAL_UINT8(0xff, image.pixels[3]);   /* A */
    jce_image_free(&image);
    jce_pak_close(pak);
}

static void test_loads_sprite_atlas_from_pak(void)
{
    static const char atlas_json[] =
        "{"
        "\"frames\":[{\"frame\":{\"x\":0,\"y\":0,\"w\":16,\"h\":16},"
        "\"duration\":80}],"
        "\"meta\":{\"image\":\"sprites/spark.png\","
        "\"frameTags\":[{\"name\":\"idle\",\"from\":0,\"to\":0}]}"
        "}";
    const JceCookInput input = {
        "sprites/spark.json", atlas_json, sizeof(atlas_json) - 1
    };
    JceCookConfig config = {0};
    void *pak_blob = NULL;
    size_t pak_size = 0;
    JcePakArchive *pak;
    JceSpriteSheet *sheet;

    TEST_ASSERT_TRUE(jce_archive_cook(&input, 1, &config, &pak_blob,
                                      &pak_size, NULL));
    pak = jce_pak_open_owned(pak_blob, pak_size);
    TEST_ASSERT_NOT_NULL(pak);
    sheet = jce_sprite_sheet_load_json_pak(pak, "sprites/spark.json", NULL);
    TEST_ASSERT_NOT_NULL(sheet);
    TEST_ASSERT_EQUAL_UINT32(1, jce_sprite_sheet_frame_count(sheet));
    TEST_ASSERT_EQUAL_UINT32(1, jce_sprite_sheet_anim_count(sheet));
    TEST_ASSERT_EQUAL_STRING("sprites/spark.png",
                             jce_sprite_sheet_image_path(sheet));
    jce_sprite_sheet_destroy(sheet);
    jce_pak_close(pak);
}

static void test_loads_sequence_from_active_pak_vfs(void)
{
    static const char sequence_json[] =
        "{\"duration\":2.0,\"fps\":30,\"loop\":true,\"tracks\":[]}";
    const JceCookInput input = {
        "sequences/intro.seq.json", sequence_json, sizeof(sequence_json) - 1
    };
    JceCookConfig config = {0};
    void *pak_blob = NULL;
    size_t pak_size = 0;
    JcePakArchive *pak;
    JceFileSystem *fs;
    JceSequencer *seq;

    TEST_ASSERT_TRUE(jce_archive_cook(&input, 1, &config, &pak_blob,
                                      &pak_size, NULL));
    pak = jce_pak_open_owned(pak_blob, pak_size);
    TEST_ASSERT_NOT_NULL(pak);
    jce_fs_set_pak_provider(&TEST_PAK_PROVIDER);
    fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    jce_fs_mount_pak(fs, pak);
    jce_fs_set_active(fs);

    seq = jce_sequencer_load_file("sequences/intro.seq.json");
    TEST_ASSERT_NOT_NULL(seq);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, jce_sequencer_duration(seq));
    jce_sequencer_free(seq);
    jce_fs_set_active(NULL);
    jce_fs_destroy(fs);
    jce_pak_close(pak);
    jce_fs_set_pak_provider(NULL);
}

typedef struct MaterialProbe {
    bool found;
    float base_color_r;
} MaterialProbe;

static void probe_mesh_material(JceScene *scene, JceEntity entity, void *user)
{
    MaterialProbe *probe = (MaterialProbe *)user;
    JceMeshRenderer *mesh = jce_scene_get_mesh_renderer(scene, entity);
    if (!mesh) return;
    probe->found = true;
    probe->base_color_r = mesh->base_color[0];
}

static void test_loads_pak_backed_material_references(void)
{
    static const char scene_json[] =
        "{"
        "\"contract\":{\"name\":\"jce.scene\",\"major\":1,\"minor\":0},"
        "\"scene\":{\"version\":1,\"entities\":[{"
        "\"id\":1,\"name\":\"Pak Material\",\"components\":[{"
        "\"type\":\"MeshRenderer\",\"meshPath\":\"models/unit.glb\","
        "\"materialPath\":\"materials/test.mat.json\""
        "}]}]}"
        "}";
    static const char material_json[] =
        "{\"type\":\"pbr\",\"baseColorFactor\":[0.25,0.5,0.75,1.0]}";
    const JceCookInput inputs[] = {
        { "scenes/material.scene.json", scene_json, sizeof(scene_json) - 1 },
        { "materials/test.mat.json", material_json, sizeof(material_json) - 1 }
    };
    JceCookConfig config = {0};
    void *pak_blob = NULL;
    size_t pak_size = 0;
    JcePakArchive *pak;
    JceFileSystem *fs;
    JceScene *scene;
    MaterialProbe probe = {0};

    TEST_ASSERT_TRUE(jce_archive_cook(inputs, 2, &config, &pak_blob,
                                      &pak_size, NULL));
    pak = jce_pak_open_owned(pak_blob, pak_size);
    TEST_ASSERT_NOT_NULL(pak);
    jce_fs_set_pak_provider(&TEST_PAK_PROVIDER);
    fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    jce_fs_mount_pak(fs, pak);
    scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);

    TEST_ASSERT_TRUE(jce_scene_serial_load_vfs(scene, fs,
                                                "scenes/material.scene.json"));
    jce_scene_each_entity(scene, probe_mesh_material, &probe);
    TEST_ASSERT_TRUE(probe.found);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.25f, probe.base_color_r);

    jce_scene_destroy(scene);

    scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity *added = NULL;
    uint32_t added_count = 0;
    probe = (MaterialProbe){0};
    TEST_ASSERT_TRUE(jce_scene_serial_load_additive_vfs(
        scene, fs, "scenes/material.scene.json", &added, &added_count));
    TEST_ASSERT_EQUAL_UINT32(1, added_count);
    TEST_ASSERT_NOT_NULL(added);
    jce_scene_each_entity(scene, probe_mesh_material, &probe);
    TEST_ASSERT_TRUE(probe.found);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.25f, probe.base_color_r);
    jce_scene_serial_free_entities(added);
    jce_scene_destroy(scene);

    jce_fs_destroy(fs);
    jce_pak_close(pak);
    jce_fs_set_pak_provider(NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_accepts_relative_startup_scene);
    RUN_TEST(test_parse_rejects_wrong_contract);
    RUN_TEST(test_parse_rejects_host_path_escape);
    RUN_TEST(test_parse_allows_explicit_empty_project);
    RUN_TEST(test_loads_manifest_from_pak);
    RUN_TEST(test_decodes_image_from_pak);
    RUN_TEST(test_loads_sprite_atlas_from_pak);
    RUN_TEST(test_loads_sequence_from_active_pak_vfs);
    RUN_TEST(test_loads_pak_backed_material_references);
    return UNITY_END();
}
