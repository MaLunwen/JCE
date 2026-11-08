/* Complete actual built-in load owners and actual invalid skinned submit. */
#include "unity.h"
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_scene_pick.h>
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/os/core/jce_log.h>
#include <bgfx/c99/bgfx.h>
#include "renderer/jce_renderer_shader_profile_internal.h"
#include "renderer/jce_skinned_mesh_deform_internal.h"
#include <jce/renderer/jce_model.h>
#include <jce/middleware/scene/jce_scene_identity.h>
#include <string.h>

struct JceScenePickPass {
    bgfx_program_handle_t prog_mesh, prog_pbr, prog_skinned;
    JceSceneRenderer *scene_renderer;
    JceRenderer *renderer;
    uint16_t view_id;
    bgfx_uniform_handle_t u_pick_id;
};
struct JceSceneRenderer {
    const JcePakArchive *pak; bool gbuffer_prog_tried; bgfx_program_handle_t prog_gbuffer;
    const JceScene *anim_storage_scene;
};
struct JceSkinnedMesh {
    bgfx_vertex_buffer_handle_t vbh; bgfx_index_buffer_handle_t ibh;
    uint32_t num_verts, num_indices;
};
static struct {
    JceRendererApiInfo api;
    unsigned api_queries, static_loads, skin_loads, gbuffer_loads, warnings;
    unsigned vertex_binds, index_binds, states, submits;
    bool fail_skin, fail_gbuffer;
    uint16_t last_submit;
    unsigned anim_lookups, bounds_calls, model_draws, model_ids;
    uint32_t joint_count;
    const jce_mat4 *palette;
    JceModelMorphVbCb vb_cb;
    void *vb_user;
} port;
static JceRendererApiInfo observed_api(void) { ++port.api_queries;return port.api; }
static JceShaderHandle named_load(const JcePakArchive *pak,const char *vs,const char *fs)
{
    TEST_ASSERT_NOT_NULL(pak);TEST_ASSERT_EQUAL_STRING("pick",fs);
    JceShaderHandle handle={UINT16_MAX};
    if(!strcmp(vs,"pick")){++port.static_loads;handle.idx=0;}
    else {TEST_ASSERT_EQUAL_STRING("pick_skinned",vs);++port.skin_loads;if(!port.fail_skin)handle.idx=1;}
    return handle;
}
static JceShaderHandle pair_load(const JcePakArchive *pak,const char *name)
{
    TEST_ASSERT_NOT_NULL(pak);TEST_ASSERT_EQUAL_STRING("gbuffer",name);++port.gbuffer_loads;
    JceShaderHandle handle={port.fail_gbuffer?UINT16_MAX:0};return handle;
}
static void counted_log(JceLogLevel level,const char *tag,const char *file,int line,const char *format,...)
{(void)tag;(void)file;(void)line;(void)format;TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_WARN,level);++port.warnings;}
#define jce_renderer_get_api_info observed_api
#define shader_load_program_named named_load
#define shader_load_program pair_load
#define jce_log_write counted_log
#define LOG_TAG "optional-stock"
#include JCE_TEST_OPTIONAL_PICK_OWNER
#include JCE_TEST_OPTIONAL_GBUFFER_OWNER

static void sm_bind_vertices(const JceSkinnedMesh *mesh,
                             const JceSkinVertexSource *source)
{
    TEST_ASSERT_NOT_NULL(mesh);
    TEST_ASSERT_NULL(source);
    ++port.vertex_binds;
}
static void bind_indices(bgfx_index_buffer_handle_t buffer,uint32_t start,uint32_t count)
{(void)buffer;(void)start;(void)count;++port.index_binds;}
static void set_state(uint64_t state,uint32_t rgba) {(void)state;(void)rgba;++port.states;}
static void jce_enc_submit(bgfx_view_id_t view,bgfx_program_handle_t program,uint32_t depth,uint8_t flags)
{(void)view;(void)depth;TEST_ASSERT_EQUAL_UINT(BGFX_DISCARD_ALL,flags);++port.submits;port.last_submit=program.idx;}
#define bgfx_set_index_buffer bind_indices
#define bgfx_set_state set_state
#undef JCE_PROFILE_ZONE_N
#undef JCE_PROFILE_ZONE_END
#define JCE_PROFILE_ZONE_N(name) do {(void)(name);} while(0)
#define JCE_PROFILE_ZONE_END do {} while(0)
#include JCE_TEST_OPTIONAL_SKINNED_SUBMIT_OWNER

typedef struct SrAnimInstance {
    JceModel *model;
    jce_mat4 skin_palette[1];
    uint32_t skin_palette_count;
} SrAnimInstance;
static SrAnimInstance live_anim;
static SrAnimInstance *sr_find_anim_instance(JceSceneRenderer *sr, JceEntity entity)
{
    TEST_ASSERT_NOT_NULL(sr);
    TEST_ASSERT_EQUAL_UINT64(7, entity);
    ++port.anim_lookups;
    return &live_anim;
}
static uint16_t fake_vb(void *user, uint32_t node, uint32_t prim)
{
    (void)user; (void)node; (void)prim;
    return 9;
}
static JceModelMorphVbCb sr_anim_geometry_vb_cb(const SrAnimInstance *anim)
{
    TEST_ASSERT_EQUAL_PTR(&live_anim, anim);
    return fake_vb;
}
static bool observed_bounds(const JceModel *model, float min[3], float max[3])
{
    (void)model; (void)min; (void)max;
    ++port.bounds_calls;
    return false;
}
static bool pick_culled(const jce_vec4 planes[6], bool have, const jce_mat4 *model,
                        const float min[3], const float max[3])
{
    (void)planes; (void)have; (void)model; (void)min; (void)max;
    TEST_FAIL_MESSAGE("No bounds are available for culling");
    return false;
}
static bool pick_register_entity(JceScenePickPass *pass, JceEntity entity, float color[4])
{
    TEST_ASSERT_NOT_NULL(pass);
    TEST_ASSERT_EQUAL_UINT64(7, entity);
    color[0] = 0.25f;
    color[1] = color[2] = 0.0f;
    color[3] = 1.0f;
    return true;
}
static void observed_uniform(bgfx_uniform_handle_t uniform, const void *values, uint16_t count)
{
    TEST_ASSERT_EQUAL_UINT(4, uniform.idx);
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, ((const float *)values)[0]);
    ++port.model_ids;
}
static void observed_model(const JceModel *model, const JceRenderer *r,
                           uint16_t view, const jce_mat4 *world, const jce_mat4 *pose,
                           uint32_t count, JceShaderHandle rigid, JceShaderHandle skin,
                           JceModelMorphVbCb vb_cb, void *vb_user,
                           void (*prepare)(void *), void *prepare_user)
{
    (void)model; (void)r; (void)view; (void)world; (void)rigid; (void)skin;
    ++port.model_draws;
    port.joint_count = count;
    port.palette = pose;
    port.vb_cb = vb_cb;
    port.vb_user = vb_user;
    TEST_ASSERT_TRUE(prepare != NULL);
    prepare(prepare_user);
}
#define bgfx_set_uniform observed_uniform
#define jce_model_get_aabb observed_bounds
#define jce_model_submit_pick_id_deformed observed_model
#include JCE_TEST_OPTIONAL_PICK_MODEL_OWNER
#undef jce_model_submit_pick_id_deformed
#undef jce_model_get_aabb
#undef bgfx_set_uniform

static unsigned pak_token,renderer_token;
static JceScenePickDesc desc(void)
{JceScenePickDesc out;memset(&out,0,sizeof out);out.pak=(const JcePakArchive *)&pak_token;return out;}
static JceSceneRenderer scene_renderer(void)
{JceSceneRenderer sr;memset(&sr,0,sizeof sr);sr.pak=(const JcePakArchive *)&pak_token;sr.prog_gbuffer.idx=UINT16_MAX;return sr;}
static void submit_skin(uint16_t program)
{
    JceSkinnedMesh mesh;memset(&mesh,0,sizeof mesh);mesh.ibh.idx=UINT16_MAX;
    jce_skinned_mesh_submit_pick_id(&mesh,(const JceRenderer *)&renderer_token,7,
        (JceShaderHandle){program},false);
}
void setUp(void)
{memset(&port,0,sizeof port);port.api.build_tier=JCE_GRAPHICS_API_TIER_STABLE;port.api.backend=JCE_BACKEND_OPENGL;}
void tearDown(void) {}
static void public_policy_and_private_actual_backend_authority_match_contract(void)
{
    TEST_ASSERT_TRUE(jce_graphics_api_is_compatibility_gl_es(JCE_GRAPHICS_API_TIER_COMPATIBILITY,JCE_BACKEND_OPENGL));
    TEST_ASSERT_TRUE(jce_graphics_api_is_compatibility_gl_es(JCE_GRAPHICS_API_TIER_COMPATIBILITY,JCE_BACKEND_OPENGLES));
    const JceRendererBackend excluded[]={JCE_BACKEND_AUTO,JCE_BACKEND_D3D11,JCE_BACKEND_D3D12,JCE_BACKEND_VULKAN,JCE_BACKEND_METAL,JCE_BACKEND_NOOP};
    for(size_t i=0;i<sizeof excluded/sizeof excluded[0];++i)
        TEST_ASSERT_FALSE(jce_graphics_api_is_compatibility_gl_es(JCE_GRAPHICS_API_TIER_COMPATIBILITY,excluded[i]));
    for(int tier=JCE_GRAPHICS_API_TIER_STABLE;tier<=JCE_GRAPHICS_API_TIER_CURRENT;++tier)
        TEST_ASSERT_FALSE(jce_graphics_api_is_compatibility_gl_es((JceGraphicsApiTier)tier,JCE_BACKEND_OPENGL));
    port.api.build_tier=JCE_GRAPHICS_API_TIER_COMPATIBILITY;port.api.backend=JCE_BACKEND_D3D11;
    TEST_ASSERT_TRUE(jce_renderer_shader_profile_compatibility(&port.api,BGFX_RENDERER_TYPE_OPENGL));
    TEST_ASSERT_TRUE(jce_renderer_shader_profile_compatibility(&port.api,BGFX_RENDERER_TYPE_OPENGLES));
    port.api.backend=JCE_BACKEND_OPENGL;
    TEST_ASSERT_FALSE(jce_renderer_shader_profile_compatibility(&port.api,BGFX_RENDERER_TYPE_DIRECT3D11));
    TEST_ASSERT_FALSE(jce_renderer_shader_profile_compatibility(NULL,BGFX_RENDERER_TYPE_OPENGL));
    TEST_ASSERT_EQUAL_UINT(0,port.api_queries);
}
static void compatibility_pick_loads_static_once_and_preserves_invalid_skin(void)
{
    port.api.build_tier=JCE_GRAPHICS_API_TIER_COMPATIBILITY;
    JceScenePickPass pass;memset(&pass,0,sizeof pass);JceScenePickDesc d=desc();
    pick_load_builtin_programs(&pass,&d);
    TEST_ASSERT_EQUAL_UINT(1,port.static_loads);TEST_ASSERT_EQUAL_UINT(0,port.skin_loads);
    TEST_ASSERT_EQUAL_UINT(0,pass.prog_mesh.idx);TEST_ASSERT_EQUAL_UINT(0,pass.prog_pbr.idx);
    TEST_ASSERT_EQUAL_UINT(UINT16_MAX,pass.prog_skinned.idx);TEST_ASSERT_EQUAL_UINT(0,port.warnings);
    submit_skin(pass.prog_skinned.idx);
    TEST_ASSERT_EQUAL_UINT(0,port.vertex_binds+port.index_binds+port.states+port.submits);
}
static void compatibility_es_pick_uses_same_public_profile(void)
{
    port.api.build_tier=JCE_GRAPHICS_API_TIER_COMPATIBILITY;port.api.backend=JCE_BACKEND_OPENGLES;
    JceScenePickPass pass;JceScenePickDesc d=desc();pick_load_builtin_programs(&pass,&d);
    TEST_ASSERT_EQUAL_UINT(1,port.static_loads);TEST_ASSERT_EQUAL_UINT(0,port.skin_loads);
    TEST_ASSERT_EQUAL_UINT(UINT16_MAX,pass.prog_skinned.idx);
}
static void modern_pick_keeps_both_actual_requests_and_static_alias(void)
{
    JceScenePickPass pass;JceScenePickDesc d=desc();pick_load_builtin_programs(&pass,&d);
    TEST_ASSERT_EQUAL_UINT(1,port.static_loads);TEST_ASSERT_EQUAL_UINT(1,port.skin_loads);
    TEST_ASSERT_EQUAL_UINT(0,pass.prog_pbr.idx);TEST_ASSERT_EQUAL_UINT(1,pass.prog_skinned.idx);
    TEST_ASSERT_EQUAL_UINT(0,port.warnings);submit_skin(pass.prog_skinned.idx);
    TEST_ASSERT_EQUAL_UINT(1,port.submits);TEST_ASSERT_EQUAL_UINT(1,port.last_submit);
}
static void compatibility_non_gl_does_not_disable_optional_programs(void)
{
    port.api.build_tier=JCE_GRAPHICS_API_TIER_COMPATIBILITY;port.api.backend=JCE_BACKEND_D3D11;
    JceScenePickPass pass;JceScenePickDesc d=desc();pick_load_builtin_programs(&pass,&d);
    TEST_ASSERT_EQUAL_UINT(1,port.skin_loads);TEST_ASSERT_EQUAL_UINT(0,port.warnings);
    JceSceneRenderer sr=scene_renderer();sr_ensure_normal_gbuffer_program(&sr,true);
    TEST_ASSERT_EQUAL_UINT(1,port.gbuffer_loads);TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(sr.prog_gbuffer));
}
static void missing_modern_skin_stays_invalid_and_never_uses_rigid_program(void)
{
    port.fail_skin=true;JceScenePickPass pass;JceScenePickDesc d=desc();pick_load_builtin_programs(&pass,&d);
    TEST_ASSERT_EQUAL_UINT(UINT16_MAX,pass.prog_skinned.idx);TEST_ASSERT_EQUAL_UINT(1,port.warnings);
    submit_skin(pass.prog_skinned.idx);TEST_ASSERT_EQUAL_UINT(0,port.submits+port.vertex_binds+port.states);
}
static void depth_only_prepass_does_not_query_or_load_normals_until_requested(void)
{
    JceSceneRenderer sr=scene_renderer();
    for(int i=0;i<20;++i)sr_ensure_normal_gbuffer_program(&sr,false);
    TEST_ASSERT_EQUAL_UINT(0,port.api_queries+port.gbuffer_loads);TEST_ASSERT_FALSE(sr.gbuffer_prog_tried);
    sr_ensure_normal_gbuffer_program(&sr,true);sr_ensure_normal_gbuffer_program(&sr,true);
    TEST_ASSERT_EQUAL_UINT(1,port.api_queries);TEST_ASSERT_EQUAL_UINT(1,port.gbuffer_loads);
    TEST_ASSERT_EQUAL_UINT(0,sr.prog_gbuffer.idx);TEST_ASSERT_EQUAL_UINT(0,port.warnings);
}
static void unsupported_normal_request_is_latched_without_asset_load(void)
{
    port.api.build_tier=JCE_GRAPHICS_API_TIER_COMPATIBILITY;JceSceneRenderer sr=scene_renderer();
    for(int i=0;i<20;++i)sr_ensure_normal_gbuffer_program(&sr,true);
    TEST_ASSERT_TRUE(sr.gbuffer_prog_tried);TEST_ASSERT_EQUAL_UINT(UINT16_MAX,sr.prog_gbuffer.idx);
    TEST_ASSERT_EQUAL_UINT(1,port.api_queries);TEST_ASSERT_EQUAL_UINT(0,port.gbuffer_loads);
    TEST_ASSERT_EQUAL_UINT(1,port.warnings);
}
static void failed_modern_normal_load_is_latched_and_depth_fallback_remains(void)
{
    port.fail_gbuffer=true;JceSceneRenderer sr=scene_renderer();
    sr_ensure_normal_gbuffer_program(&sr,true);sr_ensure_normal_gbuffer_program(&sr,true);
    TEST_ASSERT_EQUAL_UINT(1,port.gbuffer_loads);TEST_ASSERT_EQUAL_UINT(UINT16_MAX,sr.prog_gbuffer.idx);
    TEST_ASSERT_EQUAL_UINT(1,port.warnings);
    submit_skin(0);TEST_ASSERT_EQUAL_UINT(1,port.submits);TEST_ASSERT_EQUAL_UINT(0,port.last_submit);
}
static void pick_uses_current_model_geometry_and_rejects_stale_pose(void)
{
    unsigned scene_token, other_scene_token, model_token, other_model_token;
    JceScene *scene = (JceScene *)&scene_token;
    JceModel *asset = (JceModel *)&model_token;
    JceSceneRenderer sr = scene_renderer();
    JceScenePickPass pass = {0};
    pass.scene_renderer = &sr;
    pass.u_pick_id.idx = 4;
    sr.anim_storage_scene = scene;
    live_anim.model = asset;
    live_anim.skin_palette_count = 1;
    const jce_mat4 world = {0};
    const jce_vec4 planes[6] = {0};

    pick_submit_model(&pass, scene, 7, asset, &world, planes);
    TEST_ASSERT_EQUAL_UINT(1, port.anim_lookups);
    TEST_ASSERT_EQUAL_UINT(0, port.bounds_calls);
    TEST_ASSERT_EQUAL_UINT(1, port.model_draws);
    TEST_ASSERT_EQUAL_UINT(1, port.model_ids);
    TEST_ASSERT_EQUAL_UINT(1, port.joint_count);
    TEST_ASSERT_EQUAL_PTR(live_anim.skin_palette, port.palette);
    TEST_ASSERT_TRUE(port.vb_cb == fake_vb);
    TEST_ASSERT_EQUAL_PTR(&live_anim, port.vb_user);

    live_anim.model = (JceModel *)&other_model_token;
    pick_submit_model(&pass, scene, 7, asset, &world, planes);
    TEST_ASSERT_EQUAL_UINT(2, port.anim_lookups);
    TEST_ASSERT_EQUAL_UINT(1, port.bounds_calls);
    TEST_ASSERT_EQUAL_UINT(0, port.joint_count);
    TEST_ASSERT_NULL(port.palette);
    TEST_ASSERT_TRUE(port.vb_cb == NULL);

    sr.anim_storage_scene = (JceScene *)&other_scene_token;
    live_anim.model = asset;
    pick_submit_model(&pass, scene, 7, asset, &world, planes);
    TEST_ASSERT_EQUAL_UINT(2, port.anim_lookups);
    TEST_ASSERT_EQUAL_UINT(2, port.bounds_calls);
    TEST_ASSERT_EQUAL_UINT(0, port.joint_count);
    TEST_ASSERT_NULL(port.palette);
    TEST_ASSERT_TRUE(port.vb_cb == NULL);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(public_policy_and_private_actual_backend_authority_match_contract);
    RUN_TEST(compatibility_pick_loads_static_once_and_preserves_invalid_skin);
    RUN_TEST(compatibility_es_pick_uses_same_public_profile);
    RUN_TEST(modern_pick_keeps_both_actual_requests_and_static_alias);
    RUN_TEST(compatibility_non_gl_does_not_disable_optional_programs);
    RUN_TEST(missing_modern_skin_stays_invalid_and_never_uses_rigid_program);
    RUN_TEST(pick_uses_current_model_geometry_and_rejects_stale_pose);
    RUN_TEST(depth_only_prepass_does_not_query_or_load_normals_until_requested);
    RUN_TEST(unsupported_normal_request_is_latched_without_asset_load);
    RUN_TEST(failed_modern_normal_load_is_latched_and_depth_fallback_remains);
    return UNITY_END();
}
