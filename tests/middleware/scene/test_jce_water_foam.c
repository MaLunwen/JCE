#include "jce_water_field_internal.h"
#include <jce/api_scene.h>
#include <jce/os/core/jce_json.h>
#include "unity.h"
#include <string.h>
#include <math.h>
void setUp(void) {}
void tearDown(void) {}

static JceWaterFieldDesc field_desc(void)
{
    JceWaterFieldDesc d = {0};
    d.model = JCE_WATER_FIELD_FFT;
    d.fft_resolution = 32; d.fft_patch_size = 128.0f;
    d.fft_wind_speed = 10.0f; d.fft_wind_dir_x = 1.0f;
    d.fft_amplitude = 0.001f; d.fft_seed = 17;
    return d;
}
static JceWaterFftSpectrumDesc spectrum_desc(void)
{
    JceWaterFftSpectrumDesc s = {0};
    s.struct_size = sizeof(s); s.version = JCE_WATER_FFT_SPECTRUM_VERSION;
    s.model = JCE_WATER_FFT_SPECTRUM_PM;
    s.wind_speed = 10.0f; s.depth = 8.0f; s.directional_blend = 1.0f;
    return s;
}
static void test_analytic_birth_decay_and_step_subdivision(void)
{
    JceWaterFoamComponent c = jce_scene_water_foam_default();
    const double birth = 1.5, decay = 0.35, duration = 2.0;
    const double equilibrium = birth/(birth+decay);
    const double expected = equilibrium+(0.1-equilibrium)*exp(-(birth+decay)*duration);
    const float whole = jce_water_foam_integrate(0.1f, -0.5f, &c, duration);
    TEST_ASSERT_FLOAT_WITHIN(2e-7f, (float)expected, whole);
    float subdivided = 0.1f;
    for (int i=0;i<120;++i) subdivided = jce_water_foam_integrate(subdivided, -0.5f, &c, duration/120.0);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, whole, subdivided);
    c.generation_rate = 0.0f; c.decay_rate = 1.0f;
    TEST_ASSERT_FLOAT_WITHIN(1e-7f, 0.6f*(float)exp(-2.0), jce_water_foam_integrate(0.6f, -1.0f, &c, 2.0));
    TEST_ASSERT_EQUAL_FLOAT(0.6f, jce_water_foam_integrate(0.6f, -1.0f, &c, 0.0));
    c.generation_rate = 100.0f; c.decay_rate = 0.0f;
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_water_foam_integrate(0.0f, -100.0f, &c, 1e6));
    c.generation_rate = 0.0f;
    TEST_ASSERT_EQUAL_FLOAT(0.6f, jce_water_foam_integrate(0.6f, -1.0f, &c, 1e6));
}
static void test_field_clock_history_and_reconfiguration(void)
{
    JceWaterFieldSet *set = jce_water_field_set_create();
    JceWaterFieldDesc d = field_desc();
    JceWaterFftSpectrumDesc spec = spectrum_desc();
    JceWaterField *field = jce_water_field_set_acquire_spectrum(set, 7, &d, &spec);
    TEST_ASSERT_NOT_NULL(field);
    JceWaterFoamComponent c = jce_scene_water_foam_default();
    c.fold_threshold = 1.0f;
    TEST_ASSERT_TRUE(jce_water_field_set_configure_foam(set, 7, &c));
    const JceWaterFoamState *foam = jce_water_field_set_foam(set, 7);
    TEST_ASSERT_NOT_NULL(foam);
    const uint64_t identity = foam->identity;
    int driver = 0, rejected_driver = 0;
    TEST_ASSERT_FALSE(jce_water_field_set_advance(set, &rejected_driver, NAN));
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 0.5));
    foam = jce_water_field_set_foam(set, 7);
    TEST_ASSERT_EQUAL_DOUBLE(0.5, foam->time);
    int peak = 0;
    for (int i=1;i<32*32;++i) if (foam->density[0][i]>foam->density[0][peak]) peak=i;
    TEST_ASSERT_TRUE(foam->density[0][peak] > 0.0f);
    const float before = foam->density[0][peak];
    const uint64_t revision = foam->revision;
    TEST_ASSERT_FALSE(jce_water_field_set_advance(set, &rejected_driver, 1.0));
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 0.0));
    TEST_ASSERT_EQUAL_UINT64(revision, foam->revision);
    TEST_ASSERT_EQUAL_FLOAT(before, foam->density[0][peak]);
    TEST_ASSERT_TRUE(jce_water_field_set_configure_foam(set, 7, &c));
    TEST_ASSERT_EQUAL_PTR(foam, jce_water_field_set_foam(set, 7));
    c.generation_rate = 0.0f; c.decay_rate = 1.0f;
    TEST_ASSERT_TRUE(jce_water_field_set_configure_foam(set, 7, &c));
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, 1.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-7f, before*(float)exp(-1.0), foam->density[0][peak]);
    JceWaterFoamComponent invalid = c; invalid.decay_rate = NAN;
    TEST_ASSERT_FALSE(jce_water_field_set_configure_foam(set, 7, &invalid));
    TEST_ASSERT_EQUAL_PTR(foam, jce_water_field_set_foam(set, 7));
    /* Wind changes preserve the history lattice and borrowed ambient field. */
    d.fft_wind_speed = 12.0f;
    TEST_ASSERT_EQUAL_PTR(field, jce_water_field_set_acquire_spectrum(set, 7, &d, &spec));
    TEST_ASSERT_TRUE(jce_water_field_set_configure_foam(set, 7, &c));
    TEST_ASSERT_EQUAL_UINT64(identity, jce_water_field_set_foam(set, 7)->identity);
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set, &driver, -0.5));
    for (int i=0;i<32*32;++i) TEST_ASSERT_EQUAL_FLOAT(0.0f, foam->density[0][i]);
    d.fft_resolution = 64;
    TEST_ASSERT_EQUAL_PTR(field, jce_water_field_set_acquire_spectrum(set, 7, &d, &spec));
    TEST_ASSERT_NULL(jce_water_field_set_foam(set, 7));
    TEST_ASSERT_TRUE(jce_water_field_set_configure_foam(set, 7, &c));
    foam = jce_water_field_set_foam(set, 7);
    TEST_ASSERT_TRUE(foam->identity != identity);
    TEST_ASSERT_EQUAL_INT(64, foam->resolution);
    jce_water_field_set_sweep(set); jce_water_field_set_sweep(set);
    TEST_ASSERT_NULL(jce_water_field_set_foam(set, 7));
    TEST_ASSERT_NULL(jce_water_field_set_find(set, 7));
    jce_water_field_set_destroy(set);
}
static void find_water(JceScene *s, JceEntity e, void *out)
{ if (jce_scene_has_water(s,e)) *(JceEntity *)out=e; }
static void test_component_roundtrip_disable_remove_and_validation(void)
{
    JceScene *scene=jce_scene_create(), *loaded=jce_scene_create();
    JceEntity e=jce_scene_create_entity(scene,"Foam ocean");
    JceWaterComponent w={0}; w.water_mode=JCE_WATER_MODE_FFT;
    w.fft_resolution=32; w.fft_patch_size=128; w.fft_wind_speed=10;
    w.fft_wind_dir_x=1; w.fft_amplitude=0.001f;
    jce_scene_set_water(scene,e,&w);
    JceWaterSpectrumComponent spectrum=jce_scene_water_spectrum_default();
    jce_scene_set_water_spectrum(scene,e,&spectrum);
    JceWaterFoamComponent config=jce_scene_water_foam_default();
    config.generation_rate=7; config.decay_rate=0.6f; config.fold_threshold=0.4f;
    config.opacity=0.7f; config.texture_scale=0.3f;
    jce_scene_set_water_foam(scene,e,&config);
    config.struct_size+=16;
    jce_scene_set_water_foam(scene,e,&config);
    config.struct_size=sizeof(config);
    TEST_ASSERT_EQUAL_UINT32(sizeof(config),jce_scene_get_water_foam(scene,e)->struct_size);
    JceWaterFoamComponent invalid=config; invalid.version++;
    jce_scene_set_water_foam(scene,e,&invalid);
    TEST_ASSERT_EQUAL_UINT32(config.version,jce_scene_get_water_foam(scene,e)->version);
    JceJson *json=jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json); TEST_ASSERT_TRUE(jce_scene_load_json(loaded,json));
    JceEntity copy=0; jce_scene_each_entity(loaded,find_water,&copy);
    JceWaterFoamComponent *restored=jce_scene_get_water_foam(loaded,copy);
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQUAL_MEMORY(&config,restored,sizeof(config));
    JceWaterField *field=jce_scene_acquire_water_field(loaded,copy);
    TEST_ASSERT_NOT_NULL(field);
    TEST_ASSERT_NOT_NULL(jce_water_field_set_foam(jce_scene_water_fields(loaded),copy));
    int id=jce_component_find("WaterFoam"); TEST_ASSERT_TRUE(id!=JCE_COMP_ID_INVALID);
    jce_scene_set_comp_enabled(loaded,copy,id,false);
    TEST_ASSERT_EQUAL_PTR(field,jce_scene_acquire_water_field(loaded,copy));
    TEST_ASSERT_NULL(jce_water_field_set_foam(jce_scene_water_fields(loaded),copy));
    jce_scene_set_comp_enabled(loaded,copy,id,true);
    TEST_ASSERT_EQUAL_PTR(field,jce_scene_acquire_water_field(loaded,copy));
    TEST_ASSERT_NOT_NULL(jce_water_field_set_foam(jce_scene_water_fields(loaded),copy));
    jce_scene_remove_water_foam(loaded,copy);
    TEST_ASSERT_FALSE(jce_scene_has_water_foam(loaded,copy));
    TEST_ASSERT_NULL(jce_water_field_set_foam(jce_scene_water_fields(loaded),copy));
    jce_json_free(json);jce_scene_destroy(loaded);jce_scene_destroy(scene);
}
static void test_two_bodies_and_ambient_sampling_remain_independent(void)
{
    JceWaterFieldSet *set=jce_water_field_set_create(), *reference=jce_water_field_set_create();
    JceWaterFieldDesc d=field_desc();JceWaterFftSpectrumDesc spec=spectrum_desc();
    JceWaterField *a=jce_water_field_set_acquire_spectrum(set,1,&d,&spec);
    JceWaterField *pure=jce_water_field_set_acquire_spectrum(reference,1,&d,&spec);
    d.fft_seed=91;
    TEST_ASSERT_NOT_NULL(jce_water_field_set_acquire_spectrum(set,2,&d,&spec));
    JceWaterFoamComponent c=jce_scene_water_foam_default();c.fold_threshold=1;
    TEST_ASSERT_TRUE(jce_water_field_set_configure_foam(set,2,&c));
    c.generation_rate=0;
    TEST_ASSERT_TRUE(jce_water_field_set_configure_foam(set,1,&c));
    int driver=0;
    TEST_ASSERT_TRUE(jce_water_field_set_advance(set,&driver,0.5));
    TEST_ASSERT_TRUE(jce_water_field_set_advance(reference,&driver,0.5));
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_height_data(jce_water_field_fft(pure)),
        jce_water_fft_height_data(jce_water_field_fft(a)),32*32*sizeof(float));
    const JceWaterFoamState *first=jce_water_field_set_foam(set,1);
    const JceWaterFoamState *second=jce_water_field_set_foam(set,2);
    TEST_ASSERT_NOT_NULL(first);TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_TRUE(first->identity!=second->identity);
    float kept[32*32];memcpy(kept,second->density[0],sizeof(kept));
    float peak=0;
    for(int i=0;i<32*32;++i){
        TEST_ASSERT_EQUAL_FLOAT(0,first->density[0][i]);
        if(kept[i]>peak)peak=kept[i];
    }
    TEST_ASSERT_TRUE(peak>0);
    TEST_ASSERT_TRUE(jce_water_field_set_configure_foam(set,1,NULL));
    TEST_ASSERT_NULL(jce_water_field_set_foam(set,1));
    TEST_ASSERT_EQUAL_PTR(second,jce_water_field_set_foam(set,2));
    TEST_ASSERT_EQUAL_MEMORY(kept,second->density[0],sizeof(kept));
    jce_water_field_set_destroy(reference);jce_water_field_set_destroy(set);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_analytic_birth_decay_and_step_subdivision);
    RUN_TEST(test_field_clock_history_and_reconfiguration);
    RUN_TEST(test_component_roundtrip_disable_remove_and_validation);
    RUN_TEST(test_two_bodies_and_ambient_sampling_remain_independent);
    return UNITY_END();
}
