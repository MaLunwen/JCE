/* Actual public wrapper + shader-flag derivation, typed counted render/getter
 * ports and private storage. No renderer/device, copied implementation or GPU. */
#include "unity.h"
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_postfx.h>
#include <string.h>

struct JcePostFXPipeline { bool tonemap; };
struct JceSceneRenderer {
    bool force_linear_output,postfx_tonemap_active;
    JcePostFXPipeline *postfx_pipeline;
};
static JceSceneRenderer renderer;
static JcePostFXPipeline pipeline;
static unsigned char scene_token,camera_token;
static JceScene *scene;
static const JceCamera *camera;
static JceSceneRenderConfig config;
static unsigned calls,getters,depth;
static bool nested,early,observed_force,observed_linear;
static uint16_t render_port(JceSceneRenderer *sr,JceScene *s,const JceCamera *cam,
    uint16_t view,float dt,const JceSceneRenderConfig *cfg);
static bool enabled_port(const JcePostFXPipeline *p,JcePostFXType effect)
{
    TEST_ASSERT_EQUAL_PTR(&pipeline,p);
    TEST_ASSERT_EQUAL_INT(JCE_POSTFX_TONEMAP,effect);
    ++getters;return p->tonemap;
}
#define jce_scene_renderer_render render_port
#define jce_postfx_is_enabled enabled_port
#include JCE_TEST_SCENE_LINEAR_WRAPPER_OWNER

static uint16_t render_port(JceSceneRenderer *sr,JceScene *s,const JceCamera *cam,
    uint16_t view,float dt,const JceSceneRenderConfig *cfg)
{
    ++calls;
    TEST_ASSERT_EQUAL_PTR(camera,cam);
    TEST_ASSERT_EQUAL_PTR(&config,cfg);
    TEST_ASSERT_EQUAL_UINT16(40,view);
    TEST_ASSERT_EQUAL_FLOAT(0.125f,dt);
    if(!sr)return view;
    TEST_ASSERT_EQUAL_PTR(&renderer,sr);
    observed_force=sr->force_linear_output;
    if(!s || early) {
        /* The port changes derived state before an ordinary early return;
         * the complete wrapper must still restore its entry snapshot. */
        sr->postfx_tonemap_active=!sr->postfx_tonemap_active;
        return view;
    }
    TEST_ASSERT_EQUAL_PTR(scene,s);
#include JCE_TEST_SCENE_LINEAR_DERIVED_OWNER
    observed_linear=sr->postfx_tonemap_active;
    if(nested && depth==0) {
        ++depth;
        TEST_ASSERT_EQUAL_UINT16(47,jce_scene_renderer_render_linear(sr,s,cam,view,dt,cfg));
        --depth;
        TEST_ASSERT_TRUE(sr->force_linear_output);
        TEST_ASSERT_TRUE(sr->postfx_tonemap_active);
    }
    return view+7;
}
#undef jce_scene_renderer_render
#undef jce_postfx_is_enabled

void setUp(void)
{
    memset(&renderer,0,sizeof renderer);memset(&pipeline,0,sizeof pipeline);
    memset(&config,0,sizeof config);
    renderer.postfx_pipeline=&pipeline;
    scene=(JceScene *)&scene_token;camera=(const JceCamera *)&camera_token;
    calls=getters=depth=0;nested=early=observed_force=observed_linear=false;
}
void tearDown(void) {}
static uint16_t linear(JceSceneRenderer *sr,JceScene *s)
{return jce_scene_renderer_render_linear(sr,s,camera,40,0.125f,&config);}
static void null_renderer_preserves_original_call_and_view(void)
{
    TEST_ASSERT_EQUAL_UINT16(40,linear(NULL,scene));
    TEST_ASSERT_EQUAL_UINT(1,calls);TEST_ASSERT_EQUAL_UINT(0,getters);
}
static void every_previous_flag_and_pipeline_value_is_restored(void)
{
    for(unsigned force=0;force<2;++force)for(unsigned tone=0;tone<2;++tone)
        for(unsigned enabled=0;enabled<2;++enabled) {
            renderer.force_linear_output=force!=0;
            renderer.postfx_tonemap_active=tone!=0;
            pipeline.tonemap=enabled!=0;
            TEST_ASSERT_EQUAL_UINT16(47,linear(&renderer,scene));
            TEST_ASSERT_TRUE(observed_force);TEST_ASSERT_TRUE(observed_linear);
            TEST_ASSERT_EQUAL_INT(force!=0,renderer.force_linear_output);
            TEST_ASSERT_EQUAL_INT(tone!=0,renderer.postfx_tonemap_active);
            TEST_ASSERT_EQUAL_INT(enabled!=0,pipeline.tonemap);
        }
    TEST_ASSERT_EQUAL_UINT(8,calls);
}
static void all_early_returns_restore_both_private_flags(void)
{
    for(unsigned force=0;force<2;++force)for(unsigned tone=0;tone<2;++tone)
        for(unsigned route=0;route<2;++route) {
            renderer.force_linear_output=force!=0;
            renderer.postfx_tonemap_active=tone!=0;early=route!=0;
            TEST_ASSERT_EQUAL_UINT16(40,linear(&renderer,route ? scene : NULL));
            TEST_ASSERT_TRUE(observed_force);
            TEST_ASSERT_EQUAL_INT(force!=0,renderer.force_linear_output);
            TEST_ASSERT_EQUAL_INT(tone!=0,renderer.postfx_tonemap_active);
        }
    TEST_ASSERT_EQUAL_UINT(8,calls);
}
static void nested_call_and_following_main_render_keep_original_policy(void)
{
    nested=true;
    TEST_ASSERT_EQUAL_UINT16(47,linear(&renderer,scene));
    TEST_ASSERT_EQUAL_UINT(2,calls);
    TEST_ASSERT_FALSE(renderer.force_linear_output);
    TEST_ASSERT_FALSE(renderer.postfx_tonemap_active);
    nested=false;
    TEST_ASSERT_EQUAL_UINT16(47,render_port(&renderer,scene,camera,40,0.125f,&config));
    TEST_ASSERT_FALSE(observed_force);TEST_ASSERT_FALSE(observed_linear);
    TEST_ASSERT_EQUAL_UINT(3,calls);TEST_ASSERT_EQUAL_UINT(1,getters);
}
static void ordinary_and_missing_pipeline_derivation_are_preserved(void)
{
    for(unsigned enabled=0;enabled<2;++enabled) {
        pipeline.tonemap=enabled!=0;
        TEST_ASSERT_EQUAL_UINT16(47,render_port(&renderer,scene,camera,40,0.125f,&config));
        TEST_ASSERT_EQUAL_INT(enabled!=0,observed_linear);
    }
    renderer.postfx_pipeline=NULL;
    TEST_ASSERT_EQUAL_UINT16(47,linear(&renderer,scene));
    TEST_ASSERT_TRUE(observed_linear);
    TEST_ASSERT_FALSE(renderer.force_linear_output);
    TEST_ASSERT_TRUE(renderer.postfx_tonemap_active); /* entry snapshot from enabled case */
    TEST_ASSERT_EQUAL_UINT(2,getters);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(null_renderer_preserves_original_call_and_view);
    RUN_TEST(every_previous_flag_and_pipeline_value_is_restored);
    RUN_TEST(all_early_returns_restore_both_private_flags);
    RUN_TEST(nested_call_and_following_main_render_keep_original_policy);
    RUN_TEST(ordinary_and_missing_pipeline_derivation_are_preserved);
    return UNITY_END();
}
