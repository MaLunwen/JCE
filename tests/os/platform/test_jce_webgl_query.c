/* Complete private production query owner; counted context/query/error ports.
 * Sample values are provider results, never a copied capability table. */
#include "unity.h"
#include "jce_webgl_query_internal.h"
#ifndef JCE_WEBGL_QUERY_OWNER
#define JCE_WEBGL_QUERY_OWNER "jce_webgl_query_internal.c"
#endif
#include JCE_WEBGL_QUERY_OWNER

static unsigned contexts,queries,errors;
static uint32_t extensions,error_code,last_target,last_format,last_pname;
static int32_t last_count;
static bool available,empty;
static bool observe(void *user,uint32_t *flags)
{(void)user;++contexts;*flags=extensions;return available;}
static void delegate(void *user,uint32_t target,uint32_t format,uint32_t pname,
    int32_t count,int32_t *samples)
{
    (void)user;++queries;last_target=target;last_format=format;last_pname=pname;last_count=count;
    if(!empty && count>0) {samples[0]=4;if(count>1)samples[1]=2;}
}
static void report_error(void *user,uint32_t code)
{(void)user;++errors;error_code=code;}
static const JceWebGlQueryProvider provider={observe,delegate,report_error,NULL};
void setUp(void)
{
    contexts=queries=errors=0;extensions=error_code=last_target=last_format=last_pname=0;
    last_count=0;available=true;empty=false;
}
void tearDown(void) {}
static JceWebGlQueryOutcome query(uint32_t format,int32_t count,int32_t *samples)
{return jce_webgl_sample_query_internal(&provider,0x8d41u,format,0x80a9u,count,samples);}
static void test_real_provider_samples_are_preserved_and_count_forwarded(void)
{
    int32_t samples[3]={99,98,97};
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_DELEGATED,query(0x8058u,2,samples));
    TEST_ASSERT_EQUAL_INT32(4,samples[0]);TEST_ASSERT_EQUAL_INT32(2,samples[1]);
    TEST_ASSERT_EQUAL_INT32(97,samples[2]);TEST_ASSERT_EQUAL_INT32(2,last_count);
    TEST_ASSERT_EQUAL_UINT32(0x8d41u,last_target);TEST_ASSERT_EQUAL_UINT32(0x8058u,last_format);
    TEST_ASSERT_EQUAL_UINT32(0x80a9u,last_pname);TEST_ASSERT_EQUAL_UINT32(1,queries);
    TEST_ASSERT_EQUAL_UINT32(0,errors);
}
static void test_empty_samples_keep_owned_first_value_zero(void)
{
    empty=true;int32_t samples[2]={99,98};
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_DELEGATED,query(0x8058u,2,samples));
    TEST_ASSERT_EQUAL_INT32(0,samples[0]);TEST_ASSERT_EQUAL_INT32(98,samples[1]);
    TEST_ASSERT_EQUAL_UINT32(1,queries);TEST_ASSERT_EQUAL_UINT32(0,errors);
}
static void test_texture_multisample_target_refuses_without_browser_query(void)
{
    int32_t sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_ENUM,jce_webgl_sample_query_internal(
        &provider,0x9100u,0x8058u,0x80a9u,1,&sample));
    TEST_ASSERT_EQUAL_INT32(0,sample);TEST_ASSERT_EQUAL_UINT32(0,queries);
    TEST_ASSERT_EQUAL_UINT32(1,errors);TEST_ASSERT_EQUAL_UINT32(JCE_WEBGL_INVALID_ENUM,error_code);
}
static void test_compressed_format_refuses_without_browser_query(void)
{
    int32_t sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_ENUM,query(0x83f3u,1,&sample));
    TEST_ASSERT_EQUAL_UINT32(0,queries);TEST_ASSERT_EQUAL_UINT32(JCE_WEBGL_INVALID_ENUM,error_code);
}
static void test_rgb_and_rgba_query_aliases_are_real_empty_delegations(void)
{
    empty=true;int32_t sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_DELEGATED,query(0x1907u,1,&sample));
    TEST_ASSERT_EQUAL_INT32(0,sample);sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_DELEGATED,query(0x1908u,1,&sample));
    TEST_ASSERT_EQUAL_INT32(0,sample);TEST_ASSERT_EQUAL_UINT32(2,queries);
}
static void test_depth_stencil_storage_alias_is_not_a_query_alias(void)
{
    int32_t sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_ENUM,query(0x84f9u,1,&sample));
    TEST_ASSERT_EQUAL_UINT32(0,queries);
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_DELEGATED,query(0x88f0u,1,&sample));
    TEST_ASSERT_EQUAL_INT32(4,sample);TEST_ASSERT_EQUAL_UINT32(1,queries);
}
static void test_integer_renderbuffers_delegate_empty_but_rgb_integer_refuses(void)
{
    empty=true;int32_t sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_DELEGATED,query(0x8231u,1,&sample));
    TEST_ASSERT_EQUAL_INT32(0,sample);sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_DELEGATED,query(0x906fu,1,&sample));
    TEST_ASSERT_EQUAL_INT32(0,sample);
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_ENUM,query(0x8d8fu,1,&sample));
    TEST_ASSERT_EQUAL_UINT32(2,queries);
}
static void test_float_extensions_admit_only_their_real_renderbuffer_formats(void)
{
    int32_t sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_ENUM,query(0x881au,1,&sample));
    extensions=JCE_WEBGL_RBO_HALF_FLOAT;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_DELEGATED,query(0x881au,1,&sample));
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_ENUM,query(0x8814u,1,&sample));
    extensions=JCE_WEBGL_RBO_FLOAT;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_DELEGATED,query(0x8814u,1,&sample));
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_ENUM,query(0x881bu,1,&sample)); /* RGB16F */
    TEST_ASSERT_EQUAL_UINT32(2,queries);
}
static void test_norm16_extension_is_required_for_new_renderbuffer_formats(void)
{
    int32_t sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_ENUM,query(0x822au,1,&sample));
    extensions=JCE_WEBGL_RBO_NORM16;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_DELEGATED,query(0x822au,1,&sample));
    TEST_ASSERT_EQUAL_UINT32(1,queries);
}
static void test_invalid_values_keep_existing_error_semantics_before_context(void)
{
    int32_t sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_VALUE,query(0x8058u,-1,&sample));
    TEST_ASSERT_EQUAL_INT32(99,sample);TEST_ASSERT_EQUAL_UINT32(JCE_WEBGL_INVALID_VALUE,error_code);
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_VALUE,query(0x8058u,1,NULL));
    TEST_ASSERT_EQUAL_UINT32(0,contexts);TEST_ASSERT_EQUAL_UINT32(0,queries);
    TEST_ASSERT_EQUAL_UINT32(2,errors);
}
static void test_unavailable_context_is_not_an_empty_success(void)
{
    available=false;int32_t sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_UNAVAILABLE,query(0x8058u,1,&sample));
    TEST_ASSERT_EQUAL_INT32(0,sample);TEST_ASSERT_EQUAL_UINT32(0,queries);
    TEST_ASSERT_EQUAL_UINT32(JCE_WEBGL_INVALID_OPERATION,error_code);
}
static void test_unknown_pname_preserves_invalid_enum_without_delegation(void)
{
    int32_t sample=99;
    TEST_ASSERT_EQUAL_INT(JCE_WEBGL_QUERY_BAD_ENUM,jce_webgl_sample_query_internal(
        &provider,0x8d41u,0x8058u,0x9380u,1,&sample));
    TEST_ASSERT_EQUAL_UINT32(JCE_WEBGL_INVALID_ENUM,error_code);TEST_ASSERT_EQUAL_UINT32(0,queries);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_real_provider_samples_are_preserved_and_count_forwarded);
    RUN_TEST(test_empty_samples_keep_owned_first_value_zero);
    RUN_TEST(test_texture_multisample_target_refuses_without_browser_query);
    RUN_TEST(test_compressed_format_refuses_without_browser_query);
    RUN_TEST(test_rgb_and_rgba_query_aliases_are_real_empty_delegations);
    RUN_TEST(test_depth_stencil_storage_alias_is_not_a_query_alias);
    RUN_TEST(test_integer_renderbuffers_delegate_empty_but_rgb_integer_refuses);
    RUN_TEST(test_float_extensions_admit_only_their_real_renderbuffer_formats);
    RUN_TEST(test_norm16_extension_is_required_for_new_renderbuffer_formats);
    RUN_TEST(test_invalid_values_keep_existing_error_semantics_before_context);
    RUN_TEST(test_unavailable_context_is_not_an_empty_success);
    RUN_TEST(test_unknown_pname_preserves_invalid_enum_without_delegation);
    return UNITY_END();
}
