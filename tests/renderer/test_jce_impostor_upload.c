/* Actual impostor production TU, controlled backend admission/submission only. */
#include "unity.h"
#include <jce/renderer/jce_impostor.h>
#include <bgfx/c99/bgfx.h>
#include <string.h>
#include <stdlib.h>
#include "renderer/jce_instance_upload.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_views.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

static unsigned available, allocation_limit, bound, submitted, calls;
static float storage[8 * 8];
static float recorded[8 * 8];
static float dynamic_data[8 * 8];
static JceInstanceUploadOwner owner;
static unsigned fail_update, fail_create, destroyed;
static unsigned uniform_binds, atlas_binds, vertex_binds, index_binds, state_binds;
/* Bake controls exercise the actual submit/poll/shutdown producer. */
#include <jce/os/core/jce_alloc.h>
#include "os/core/jce_memory.h"
static unsigned bake_texture_next, bake_texture_destroys, bake_fbo_destroys;
static unsigned bake_reads, bake_pixel_frees, bake_all_frees;
static uint32_t bake_current_frame,bake_ready_frame;
static bool bake_textures[128], bake_fbo_live, bake_borrowed, bake_backend_joined;
static bool bake_fail_fbo;
static uint16_t bake_color, bake_depth;
static void *bake_destination;
static bool controlled_bounds(const JceModel *model, float mn[3], float mx[3])
{
    (void)model;
    for (unsigned i=0; i<3; ++i) { mn[i]=0; mx[i]=1; }
    return true;
}
static bgfx_texture_handle_t controlled_bake_texture(uint16_t w, uint16_t h,
    bool mips, uint16_t layers, bgfx_texture_format_t format, uint64_t flags,
    const bgfx_memory_t *memory, uint16_t skip)
{
    (void)w; (void)h; (void)mips; (void)layers; (void)memory; (void)skip;
    TEST_ASSERT_TRUE(bake_texture_next < 128);
    uint16_t idx=(uint16_t)bake_texture_next++;
    bake_textures[idx]=true;
    if (flags & BGFX_TEXTURE_RT) {
        if (format == BGFX_TEXTURE_FORMAT_RGBA8) bake_color=idx;
        else bake_depth=idx;
    }
    return (bgfx_texture_handle_t){idx};
}
static bgfx_frame_buffer_handle_t controlled_bake_fbo(uint8_t count,
    const bgfx_attachment_t *attachments, bool owns)
{
    (void)attachments;
    TEST_ASSERT_EQUAL_UINT8(2,count); TEST_ASSERT_TRUE(owns);
    if (bake_fail_fbo) return (bgfx_frame_buffer_handle_t){UINT16_MAX};
    TEST_ASSERT_FALSE(bake_fbo_live); bake_fbo_live=true;
    return (bgfx_frame_buffer_handle_t){0}; /* Valid owned handle zero. */
}
static void controlled_bake_destroy_fbo(bgfx_frame_buffer_handle_t handle)
{
    TEST_ASSERT_EQUAL_UINT16(0,handle.idx); TEST_ASSERT_TRUE(bake_fbo_live);
    TEST_ASSERT_TRUE(bake_textures[bake_color] && bake_textures[bake_depth]);
    bake_fbo_live=false; bake_textures[bake_color]=bake_textures[bake_depth]=false;
    ++bake_fbo_destroys;
}
static void controlled_bake_destroy_texture(bgfx_texture_handle_t handle)
{
    TEST_ASSERT_TRUE(handle.idx < 128 && bake_textures[handle.idx]);
    bake_textures[handle.idx]=false; ++bake_texture_destroys;
}
static uint32_t controlled_bake_read(bgfx_texture_handle_t handle, void *pixels,
                                     uint8_t mip, uint8_t layer)
{
    (void)mip; (void)layer;
    TEST_ASSERT_TRUE(handle.idx < 128 && bake_textures[handle.idx]);
    TEST_ASSERT_NOT_NULL(pixels);
    bake_destination=pixels; bake_borrowed=true; ++bake_reads;
    return bake_ready_frame; /* Independently controlled provider promise. */
}
static void controlled_bake_free(void *data)
{
    if (!data) return;
    if (data == bake_destination) {
        TEST_ASSERT_TRUE_MESSAGE(!bake_borrowed || bake_backend_joined,
                                  "impostor CPU destination freed before backend join");
        ++bake_pixel_frees; bake_destination=NULL; bake_borrowed=false;
    }
    ++bake_all_frees;
    jce_free(data);
}
#undef JCE_FREE
#define JCE_FREE controlled_bake_free


static uint32_t controlled_available(uint32_t count, uint16_t stride)
{
    TEST_ASSERT_EQUAL_UINT16(32, stride);
    return count < available ? count : available;
}

static void controlled_allocate(bgfx_instance_data_buffer_t *out,
                                uint32_t count, uint16_t stride)
{
    memset(out, 0, sizeof(*out));
    out->num = count < allocation_limit ? count : allocation_limit;
    out->stride = stride;
    out->data = (uint8_t *)storage;
    memset(storage, 0xA5, sizeof(storage));
}

static void controlled_bind(const bgfx_instance_data_buffer_t *buffer,
                            uint32_t start, uint32_t count)
{
    TEST_ASSERT_TRUE(count <= buffer->num);
    TEST_ASSERT_EQUAL_UINT32(0, start);
    TEST_ASSERT_TRUE(submitted + count <= 8);
    memcpy(recorded + submitted * 8, buffer->data, count * 32);
    bound = count;
}

static void controlled_submit(bgfx_view_id_t view, bgfx_program_handle_t program,
                              uint32_t depth, uint8_t flags)
{
    TEST_ASSERT_EQUAL_UINT16(7, view);
    (void)program; (void)depth; (void)flags;
    submitted += bound;
    bound = 0;
    ++calls;
}

/* Unused atlas/bake dependencies stay outside the controlled draw witness. */
#define shader_load_program(...) ((JceShaderHandle){UINT16_MAX})
#define jce_shaders_embedded_engine_pak(...) (NULL)
#define jce_model_get_aabb controlled_bounds
#define jce_model_draw(...) ((void)0)
#define jce_model_set_draw_lod(...) ((void)0)
#define jce_model_set_material_override(...) ((void)0)
#define jce_model_set_pre_submit_cb(...) ((void)0)
#define jce_renderer_get_frame_index(...) (bake_current_frame)
#define jce_view_bands_claim(...) ((void)0)
#define SDL_CreateSurfaceFrom(...) (NULL)
#define SDL_DestroySurface(...) ((void)0)
#define SDL_GetError(...) ("controlled unused bake")
#define IMG_SavePNG(...) (false)
#define bgfx_get_renderer_type(...) (BGFX_RENDERER_TYPE_NOOP)
#define bgfx_copy(...) (NULL)
#define bgfx_create_vertex_buffer(...) ((bgfx_vertex_buffer_handle_t){UINT16_MAX})
#define bgfx_create_index_buffer(...) ((bgfx_index_buffer_handle_t){UINT16_MAX})
#define bgfx_create_uniform(...) ((bgfx_uniform_handle_t){UINT16_MAX})
#define bgfx_create_texture_2d controlled_bake_texture
#define bgfx_create_frame_buffer_from_attachment controlled_bake_fbo
#define bgfx_get_texture(...) ((bgfx_texture_handle_t){bake_color})
#define bgfx_read_texture controlled_bake_read
#define bgfx_frame(...) (0u)
#define bgfx_attachment_init(...) ((void)0)
#define bgfx_vertex_layout_begin(...) ((void)0)
#define bgfx_vertex_layout_add(...) ((void)0)
#define bgfx_vertex_layout_end(...) ((void)0)
#define bgfx_destroy_frame_buffer controlled_bake_destroy_fbo
#define bgfx_destroy_index_buffer(...) ((void)0)
#define bgfx_destroy_vertex_buffer(...) ((void)0)
#define bgfx_destroy_uniform(...) ((void)0)
#define bgfx_destroy_texture controlled_bake_destroy_texture
#define bgfx_destroy_program(...) ((void)0)
#define bgfx_set_view_clear(...) ((void)0)
#define bgfx_set_view_frame_buffer(...) ((void)0)
#define bgfx_set_view_mode(...) ((void)0)
#define bgfx_set_view_rect(...) ((void)0)
#define bgfx_set_view_transform(...) ((void)0)
#define bgfx_touch(...) ((void)0)
#define bgfx_blit(...) ((void)0)
#define jce_impostor_oct_encode controlled_impostor_oct_encode
#define jce_impostor_oct_decode controlled_impostor_oct_decode
static uint32_t provider_available(void *user, uint32_t count, uint16_t stride)
{
    (void)user;
    return controlled_available(count, stride);
}
static bool provider_allocate(void *user, uint32_t count, uint16_t stride,
                              JceInstanceTransient *out)
{
    (void)user;
    bgfx_instance_data_buffer_t buffer;
    controlled_allocate(&buffer, count, stride);
    memset(out, 0, sizeof(*out));
    out->data = buffer.data;
    out->num = buffer.num;
    out->size = buffer.num * stride;
    out->stride = stride;
    out->handle = 1;
    return true;
}
static uint16_t provider_create(void *user, uint32_t count, uint16_t stride)
{
    (void)user; (void)count; (void)stride;
    return fail_create ? UINT16_MAX : 2;
}
static void provider_destroy(void *user, uint16_t handle)
{
    (void)user; (void)handle;
    ++destroyed;
}
static void *provider_staging(void *user, uint32_t bytes)
{
    (void)user;
    return malloc(bytes);
}
static void provider_free(void *user, void *data)
{
    (void)user;
    free(data);
}
static bool provider_update(void *user, uint16_t handle, uint32_t start,
                            void *data, uint32_t bytes)
{
    (void)user; (void)handle;
    if (fail_update) return false;
    TEST_ASSERT_TRUE(start * 32 + bytes <= sizeof(dynamic_data));
    memcpy((uint8_t *)dynamic_data + start * 32, data, bytes);
    free(data);
    return true;
}
static void provider_bind_dynamic(void *user, void *encoder, uint16_t handle,
                                 uint32_t start, uint32_t count)
{
    (void)user; (void)encoder; (void)handle;
    bgfx_instance_data_buffer_t buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.num = count;
    buffer.data = (uint8_t *)(dynamic_data + start * 8);
    controlled_bind(&buffer, 0, count);
}
static void provider_bind_transient(void *user, void *encoder,
                                   const JceInstanceTransient *data, uint32_t count)
{
    (void)user; (void)encoder;
    bgfx_instance_data_buffer_t buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.num = data->num;
    buffer.data = data->data;
    controlled_bind(&buffer, 0, count);
}
static void provider_warn(void *user, uint32_t epoch, uint32_t wanted,
                          uint32_t accepted, uint16_t stride)
{
    (void)user; (void)epoch; (void)wanted; (void)accepted; (void)stride;
}
static const JceInstanceUploadProvider provider = {
    provider_available, provider_allocate, provider_create, provider_destroy,
    provider_staging, provider_free, provider_update,
    provider_bind_dynamic, provider_bind_transient, provider_warn
};
static JceInstanceUploadOwner *controlled_owner(const JceRenderer *renderer)
{
    (void)renderer;
    return &owner;
}
static uint32_t controlled_epoch(void) { return 1; }
#define jce_renderer_instance_upload_owner controlled_owner
#define jce_renderer_instance_upload_epoch controlled_epoch
#define bgfx_discard(...) ((void)0)
#define bgfx_get_avail_instance_data_buffer controlled_available
#define bgfx_alloc_instance_data_buffer controlled_allocate
#define bgfx_set_instance_data_buffer controlled_bind
#define bgfx_submit controlled_submit
#define bgfx_set_uniform(...) ((void)++uniform_binds)
#define bgfx_set_texture(...) ((void)++atlas_binds)
#define bgfx_set_vertex_buffer(...) ((void)++vertex_binds)
#define bgfx_set_index_buffer(...) ((void)++index_binds)
#define bgfx_set_state(...) ((void)++state_binds)
#define jce_impostor_card_place controlled_impostor_card_place
#define jce_impostor_meta_write controlled_impostor_meta_write
#define jce_impostor_meta_read controlled_impostor_meta_read
#define jce_impostor_atlas_load controlled_impostor_atlas_load
#define jce_impostor_draw_instanced controlled_impostor_draw_instanced
#define jce_impostor_bake_submit controlled_impostor_bake_submit
#define jce_impostor_bake_in_flight controlled_impostor_bake_in_flight
#define jce_impostor_bake_poll controlled_impostor_bake_poll
#define jce_impostor_bake_progress controlled_impostor_bake_progress
#define jce_impostor_shutdown controlled_impostor_shutdown
#include "../../engine/src/renderer/jce_impostor.c"

void setUp(void)
{
    fail_update = fail_create = destroyed = 0;
    uniform_binds = atlas_binds = vertex_binds = index_binds = state_binds = 0;
    jce_instance_upload_owner_init(&owner, &provider, NULL, 0, 1);
    available = allocation_limit = 8;
    submitted = bound = calls = 0;
    memset(recorded, 0, sizeof(recorded));
    memset(&g_gfx, 0, sizeof(g_gfx));
    g_gfx.init_tried = true;
    g_gfx.prog.idx = 1;
    g_gfx.quad_vb.idx = g_gfx.quad_ib.idx = UINT16_MAX;
    g_gfx.u_atlas.idx = g_gfx.u_lightDir.idx = g_gfx.u_lightColor.idx = UINT16_MAX;
    bake_texture_next=20; bake_color=bake_depth=UINT16_MAX;
    bake_texture_destroys=bake_fbo_destroys=bake_reads=bake_pixel_frees=bake_all_frees=0;
    memset(bake_textures,0,sizeof bake_textures);
    bake_fbo_live=bake_borrowed=bake_backend_joined=bake_fail_fbo=false;
    bake_destination=NULL;
    bake_current_frame=0;bake_ready_frame=8;
}
void tearDown(void)
{
    controlled_impostor_shutdown();
    bake_backend_joined=true;
    jce_impostor_after_backend_shutdown();
    TEST_ASSERT_FALSE(bake_borrowed);
    TEST_ASSERT_FALSE(bake_fbo_live);
    for(unsigned i=0;i<128;++i) TEST_ASSERT_FALSE(bake_textures[i]);
    jce_instance_upload_owner_destroy(&owner);
}

static void draw(void)
{
    JceImpostorAtlas atlas;
    JceImpostorInstance cards[5];
    memset(&atlas, 0, sizeof(atlas));
    atlas.valid = true;
    atlas.meta.grid_n = 8;
    for (unsigned i = 0; i < 5; ++i) {
        cards[i].center[0] = (float)i;
        cards[i].center[1] = (float)i + 10;
        cards[i].center[2] = (float)i + 20;
        cards[i].radius = (float)i + 1;
        cards[i].tint[0] = .1f;
        cards[i].tint[1] = .2f;
        cards[i].tint[2] = .3f;
    }
    controlled_impostor_draw_instanced((JceRenderer *)&atlas, 7, &atlas,
                                      cards, 5, NULL, NULL);
}

static void exact_payload(void)
{
    TEST_ASSERT_EQUAL_UINT32(5, submitted);
    for (unsigned i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_FLOAT((float)i, recorded[i * 8]);
        TEST_ASSERT_EQUAL_FLOAT((float)i + 10, recorded[i * 8 + 1]);
        TEST_ASSERT_EQUAL_FLOAT((float)i + 20, recorded[i * 8 + 2]);
        TEST_ASSERT_EQUAL_FLOAT((float)i + 1, recorded[i * 8 + 3]);
        TEST_ASSERT_EQUAL_FLOAT(8, recorded[i * 8 + 4]);
        TEST_ASSERT_EQUAL_FLOAT(.1f, recorded[i * 8 + 5]);
        TEST_ASSERT_EQUAL_FLOAT(.2f, recorded[i * 8 + 6]);
        TEST_ASSERT_EQUAL_FLOAT(.3f, recorded[i * 8 + 7]);
    }
}

static void test_full_payload(void) { draw(); exact_payload(); }
static void test_zero_transient_must_not_silently_drop(void)
{
    available = 0;
    draw();
    exact_payload();
}
static void test_partial_transient_must_preserve_remainder(void)
{
    available = 2;
    draw();
    exact_payload();
}
static void test_allocation_clamp_must_not_overwrite(void)
{
    allocation_limit = 1;
    draw();
    exact_payload();
}
static void test_invalid_inputs_do_not_submit(void)
{
    controlled_impostor_draw_instanced(NULL, 7, NULL, NULL, 5, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, submitted);
    TEST_ASSERT_EQUAL_UINT32(0, calls);
}
static void test_partial_chunks_rebind_and_keep_exact_remainder(void)
{
    available = allocation_limit = 2;
    fail_create = 1;
    draw();
    exact_payload();
    TEST_ASSERT_EQUAL_UINT32(3, calls);
    TEST_ASSERT_EQUAL_UINT32(6, uniform_binds);
    TEST_ASSERT_EQUAL_UINT32(3, atlas_binds);
    TEST_ASSERT_EQUAL_UINT32(3, vertex_binds);
    TEST_ASSERT_EQUAL_UINT32(3, index_binds);
    TEST_ASSERT_EQUAL_UINT32(3, state_binds);
}
static void test_update_failure_does_not_submit_and_retry_succeeds(void)
{
    available = 0;
    fail_update = 1;
    draw();
    TEST_ASSERT_EQUAL_UINT32(0, submitted);
    TEST_ASSERT_FALSE(owner.pending_valid);
    fail_update = 0;
    draw();
    exact_payload();
}
static void test_shared_budget_exhaustion_is_not_silent_prefix_success(void)
{
    available = 0;
    /* Other qualified lanes may already own the full shared allocation. */
    owner.owned_bytes = JCE_INSTANCE_UPLOAD_BUDGET_BYTES;
    draw();
    TEST_ASSERT_EQUAL_UINT32(0, submitted);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_ERROR_AGGREGATE_LIMIT, owner.last_error);
    TEST_ASSERT_FALSE(owner.pending_valid);
    owner.owned_bytes = 0;
    draw();
    exact_payload();
}
static JceImpostorBakeDesc bake_description(void)
{
    JceImpostorBakeDesc desc={0};
    desc.model=(JceModel *)&owner; desc.renderer=(JceRenderer *)&owner;
    desc.grid_n=2; desc.cell_px=32;
    strcpy(desc.atlas_path_host,"controlled-atlas.png");
    strcpy(desc.meta_path_host,"controlled-meta.json");
    return desc;
}
static void test_pending_bake_shutdown_retains_cpu_until_backend_join_and_resets_session(void)
{
    JceImpostorBakeDesc desc=bake_description();
    TEST_ASSERT_TRUE(controlled_impostor_bake_submit(&desc));
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_READBACK,controlled_impostor_bake_poll());
    TEST_ASSERT_EQUAL_UINT32(1,bake_reads);
    void *pixels=bake_destination;
    controlled_impostor_shutdown();
    TEST_ASSERT_EQUAL_PTR(pixels,bake_destination);
    TEST_ASSERT_EQUAL_UINT32(0,bake_pixel_frees);
    TEST_ASSERT_EQUAL_UINT32(1,bake_fbo_destroys);
    TEST_ASSERT_EQUAL_UINT32(1,bake_texture_destroys);
    TEST_ASSERT_NULL(g_bake.desc.renderer);
    TEST_ASSERT_NULL(g_bake.desc.model);
    TEST_ASSERT_TRUE(g_bake.retiring);
    TEST_ASSERT_FALSE(controlled_impostor_bake_submit(&desc));
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_READBACK,controlled_impostor_bake_poll());
    controlled_impostor_shutdown();
    TEST_ASSERT_EQUAL_UINT32(0,bake_pixel_frees);
    /* Simulate the backend's final borrowed write and actual join. */
    memset(pixels,0x5a,64u*64u*4u);
    bake_backend_joined=true;
    jce_impostor_after_backend_shutdown();
    TEST_ASSERT_EQUAL_UINT32(1,bake_pixel_frees);
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_IDLE,controlled_impostor_bake_poll());
    TEST_ASSERT_FALSE(controlled_impostor_bake_in_flight());
    TEST_ASSERT_NULL(g_bake.pixels);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,g_bake.fbo.idx);
    jce_impostor_after_backend_shutdown();
    TEST_ASSERT_EQUAL_UINT32(1,bake_pixel_frees);
    /* A later session can submit again; unissued pixels release immediately. */
    bake_backend_joined=false;
    TEST_ASSERT_TRUE(controlled_impostor_bake_submit(&desc));
    controlled_impostor_shutdown();
    TEST_ASSERT_FALSE(controlled_impostor_bake_in_flight());
    TEST_ASSERT_EQUAL_UINT32(2,bake_all_frees);
}
static void test_bake_frame_wrap_consumes_only_at_promised_cpu_delivery(void)
{
    bake_current_frame=UINT32_MAX;bake_ready_frame=1;
    JceImpostorBakeDesc desc=bake_description();
    TEST_ASSERT_TRUE(controlled_impostor_bake_submit(&desc));
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_READBACK,controlled_impostor_bake_poll());
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_READBACK,controlled_impostor_bake_poll());
    bake_current_frame=0;
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_READBACK,controlled_impostor_bake_poll());
    TEST_ASSERT_EQUAL_UINT32(0,bake_pixel_frees);TEST_ASSERT_TRUE(bake_borrowed);
    memset(bake_destination,0x5a,64u*64u*4u);bake_borrowed=false;bake_current_frame=1;
    /* The deliberately unavailable SDL encoder fails after actual consumption. */
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_FAILED,controlled_impostor_bake_poll());
    TEST_ASSERT_EQUAL_UINT32(1,bake_pixel_frees);TEST_ASSERT_EQUAL_UINT32(1,bake_fbo_destroys);
    bake_current_frame=2;
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_FAILED,controlled_impostor_bake_poll());
    TEST_ASSERT_EQUAL_UINT32(1,bake_pixel_frees);
}
static void test_bake_half_range_retains_cpu_borrow_until_backend_stop(void)
{
    bake_ready_frame=UINT32_C(0x80000000);
    JceImpostorBakeDesc desc=bake_description();
    TEST_ASSERT_TRUE(controlled_impostor_bake_submit(&desc));
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_READBACK,controlled_impostor_bake_poll());
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_READBACK,controlled_impostor_bake_poll());
    TEST_ASSERT_EQUAL_UINT32(0,bake_pixel_frees);TEST_ASSERT_TRUE(bake_borrowed);
    controlled_impostor_shutdown();TEST_ASSERT_EQUAL_UINT32(0,bake_pixel_frees);
    bake_backend_joined=true;jce_impostor_after_backend_shutdown();
    TEST_ASSERT_EQUAL_UINT32(1,bake_pixel_frees);TEST_ASSERT_FALSE(bake_borrowed);
}
static void test_unissued_bake_shutdown_releases_once_without_waiting(void)
{
    JceImpostorBakeDesc desc=bake_description();
    TEST_ASSERT_TRUE(controlled_impostor_bake_submit(&desc));
    controlled_impostor_shutdown();
    TEST_ASSERT_EQUAL_UINT32(0,bake_reads);
    TEST_ASSERT_EQUAL_UINT32(1,bake_all_frees);
    TEST_ASSERT_EQUAL_UINT32(1,bake_fbo_destroys);
    TEST_ASSERT_EQUAL_UINT32(1,bake_texture_destroys);
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_IDLE,controlled_impostor_bake_poll());
    controlled_impostor_shutdown();
    jce_impostor_after_backend_shutdown();
    TEST_ASSERT_EQUAL_UINT32(1,bake_all_frees);
}
static void test_fbo_creation_failure_releases_untransferred_attachments_once(void)
{
    JceImpostorBakeDesc desc=bake_description();
    bake_fail_fbo=true;
    TEST_ASSERT_FALSE(controlled_impostor_bake_submit(&desc));
    TEST_ASSERT_EQUAL_UINT32(2,bake_texture_destroys);
    TEST_ASSERT_EQUAL_UINT32(0,bake_fbo_destroys);
    TEST_ASSERT_EQUAL_UINT32(0,bake_reads);
    controlled_impostor_shutdown();
    jce_impostor_after_backend_shutdown();
    TEST_ASSERT_EQUAL_UINT32(2,bake_texture_destroys);
    TEST_ASSERT_EQUAL_INT(JCE_IMPOSTOR_BAKE_IDLE,controlled_impostor_bake_poll());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bake_frame_wrap_consumes_only_at_promised_cpu_delivery);
    RUN_TEST(test_bake_half_range_retains_cpu_borrow_until_backend_stop);
    RUN_TEST(test_full_payload);
    RUN_TEST(test_zero_transient_must_not_silently_drop);
    RUN_TEST(test_partial_transient_must_preserve_remainder);
    RUN_TEST(test_allocation_clamp_must_not_overwrite);
    RUN_TEST(test_invalid_inputs_do_not_submit);
    RUN_TEST(test_partial_chunks_rebind_and_keep_exact_remainder);
    RUN_TEST(test_update_failure_does_not_submit_and_retry_succeeds);
    RUN_TEST(test_shared_budget_exhaustion_is_not_silent_prefix_success);
    RUN_TEST(test_pending_bake_shutdown_retains_cpu_until_backend_join_and_resets_session);
    RUN_TEST(test_unissued_bake_shutdown_releases_once_without_waiting);
    RUN_TEST(test_fbo_creation_failure_releases_untransferred_attachments_once);
    return UNITY_END();
}
