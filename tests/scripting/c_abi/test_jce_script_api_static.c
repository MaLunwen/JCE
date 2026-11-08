/* Static call-down transport: link the actual generated API, not a loader. */
#include "unity.h"

#include <jce/middleware/script/jce_script.h>
#include <jce/script_api/jce_script_api.h>

#include <stddef.h>
#include <string.h>

typedef struct HostObservation {
    int quit_calls;
    int flush_calls;
    bool flush_result;
} HostObservation;

void setUp(void) { }
void tearDown(void) { }

static bool observed_quit(void *user)
{
    HostObservation *observed = (HostObservation *)user;
    ++observed->quit_calls;
    return true;
}

static bool observed_flush(void *user)
{
    HostObservation *observed = (HostObservation *)user;
    ++observed->flush_calls;
    return observed->flush_result;
}

static void test_static_forwarders_use_copied_host_and_user(void)
{
    HostObservation observed = {0, 0, true};
    JceScriptHost host;
    JceScriptApi *api;

    memset(&host, 0, sizeof host);
    host.user = &observed;
    host.request_quit = observed_quit;
    host.stat_flush = observed_flush;
    TEST_ASSERT_EQUAL_UINT32(JCE_SCRIPT_API_VERSION, jce_script_api_version());
    api = jce_script_api_open(&host, sizeof host, JCE_SCRIPT_API_VERSION);
    TEST_ASSERT_NOT_NULL(api);
    /* The API owns a copy: changing the caller's table cannot remove slots. */
    host.request_quit = NULL;
    host.user = NULL;
    TEST_ASSERT_TRUE(jce_script_api_request_quit(api));
    TEST_ASSERT_TRUE(jce_script_api_stat_flush(api));
    observed.flush_result = false;
    TEST_ASSERT_FALSE(jce_script_api_stat_flush(api));
    TEST_ASSERT_EQUAL_INT(1, observed.quit_calls);
    TEST_ASSERT_EQUAL_INT(2, observed.flush_calls);
    jce_script_api_close(api);
}

static void test_short_host_excludes_real_tail_callback(void)
{
    HostObservation observed = {0, 0, true};
    JceScriptHost host;
    JceScriptApi *api;
    const size_t short_size = offsetof(JceScriptHost, stat_flush);

    memset(&host, 0, sizeof host);
    host.user = &observed;
    host.request_quit = observed_quit;
    host.stat_flush = observed_flush;
    TEST_ASSERT_TRUE(short_size < sizeof host);
    api = jce_script_api_open(&host, short_size, JCE_SCRIPT_API_VERSION);
    TEST_ASSERT_NOT_NULL(api);
    TEST_ASSERT_TRUE(jce_script_api_request_quit(api));
    TEST_ASSERT_FALSE(jce_script_api_stat_flush(api));
    TEST_ASSERT_EQUAL_INT(1, observed.quit_calls);
    TEST_ASSERT_EQUAL_INT(0, observed.flush_calls);
    jce_script_api_close(api);
}

static void test_invalid_handshake_and_missing_callbacks_fail_closed(void)
{
    JceScriptHost host;
    JceScriptApi *api;

    memset(&host, 0, sizeof host);
    TEST_ASSERT_NULL(jce_script_api_open(NULL, sizeof host, JCE_SCRIPT_API_VERSION));
    TEST_ASSERT_NULL(jce_script_api_open(&host, 0, JCE_SCRIPT_API_VERSION));
    TEST_ASSERT_NULL(jce_script_api_open(&host, sizeof host, JCE_SCRIPT_API_VERSION + 1u));
    TEST_ASSERT_FALSE(jce_script_api_request_quit(NULL));
    TEST_ASSERT_FALSE(jce_script_api_stat_flush(NULL));
    jce_script_api_close(NULL);
    api = jce_script_api_open(&host, sizeof host, JCE_SCRIPT_API_VERSION);
    TEST_ASSERT_NOT_NULL(api);
    TEST_ASSERT_FALSE(jce_script_api_request_quit(api));
    TEST_ASSERT_FALSE(jce_script_api_stat_flush(api));
    jce_script_api_close(api);
}

static void observed_viewport(void *user, float out[2])
{
    int *calls = user; ++*calls; out[0]=909; out[1]=650;
}
static void test_current_viewport_tail_respects_sized_host(void)
{
    int calls=0;float size[2]={77,88};JceScriptHost host={0};
    host.user=&calls;host.viewport_size=observed_viewport;
    JceScriptApi *api=jce_script_api_open(&host,sizeof host,JCE_SCRIPT_API_VERSION);
    TEST_ASSERT_NOT_NULL(api);jce_script_api_get_viewport_size(api,size);
    TEST_ASSERT_EQUAL_FLOAT(909,size[0]);TEST_ASSERT_EQUAL_FLOAT(650,size[1]);
    TEST_ASSERT_EQUAL_INT(1,calls);jce_script_api_close(api);
    api=jce_script_api_open(&host,offsetof(JceScriptHost,viewport_size),JCE_SCRIPT_API_VERSION);
    TEST_ASSERT_NOT_NULL(api);size[0]=77;size[1]=88;
    jce_script_api_get_viewport_size(api,size);
    TEST_ASSERT_EQUAL_FLOAT(0,size[0]);TEST_ASSERT_EQUAL_FLOAT(0,size[1]);
    TEST_ASSERT_EQUAL_INT(1,calls);jce_script_api_close(api);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_current_viewport_tail_respects_sized_host);
    RUN_TEST(test_static_forwarders_use_copied_host_and_user);
    RUN_TEST(test_short_host_excludes_real_tail_callback);
    RUN_TEST(test_invalid_handshake_and_missing_callbacks_fail_closed);
    return UNITY_END();
}
