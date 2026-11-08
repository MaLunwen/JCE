/* Complete selected original WASAPI function; only external effects are ports. */
#include "unity.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <stdbool.h>
#include <miniaudio.h>
#include <stdio.h>
#include <string.h>

/* Opaque native endpoints are borrowed only by the typed ports below. */
typedef struct { int token; } ma_IMMDeviceEnumerator;
typedef struct { int token; } ma_IMMDevice;
static const GUID MA_CLSID_MMDeviceEnumerator = {0};
static const GUID MA_IID_IMMDeviceEnumerator = {1};
enum { ma_eRender = 0, ma_eCapture = 1, ma_eConsole = 0 };

static ma_context context;
static ma_IMMDeviceEnumerator enumerator;
static ma_IMMDevice device;
static struct {
    HRESULT initialize_result, create_result, endpoint_result;
    int references;
    unsigned initializes, uninitializes, creates, endpoints, gets, releases, logs;
    DWORD coinit_flags;
    int flow;
    const WCHAR *device_id;
} port;

static HRESULT port_initialize(ma_context *ctx, void *reserved, DWORD flags)
{
    TEST_ASSERT_EQUAL_PTR(&context, ctx);
    TEST_ASSERT_NULL(reserved);
    ++port.initializes;
    port.coinit_flags = flags;
    /* Native COM contract: S_OK and S_FALSE both acquire one caller reference. */
    if (port.initialize_result == S_OK || port.initialize_result == S_FALSE)
        ++port.references;
    return port.initialize_result;
}
static void port_uninitialize(ma_context *ctx)
{
    TEST_ASSERT_EQUAL_PTR(&context, ctx);
    ++port.uninitializes;
    --port.references;
}
static HRESULT port_create(ma_context *ctx, const GUID *clsid, void *outer,
                           DWORD flags, const GUID *iid, void **out)
{
    TEST_ASSERT_EQUAL_PTR(&context, ctx);
    TEST_ASSERT_EQUAL_PTR(&MA_CLSID_MMDeviceEnumerator, clsid);
    TEST_ASSERT_EQUAL_PTR(&MA_IID_IMMDeviceEnumerator, iid);
    TEST_ASSERT_NULL(outer);
    TEST_ASSERT_EQUAL_UINT(CLSCTX_ALL, flags);
    ++port.creates;
    *out = FAILED(port.create_result) ? NULL : &enumerator;
    return port.create_result;
}
static HRESULT port_default(ma_IMMDeviceEnumerator *value, int flow,
                            int role, ma_IMMDevice **out)
{
    TEST_ASSERT_EQUAL_PTR(&enumerator, value);
    TEST_ASSERT_EQUAL_INT(ma_eConsole, role);
    ++port.endpoints; port.flow = flow;
    *out = FAILED(port.endpoint_result) ? NULL : &device;
    return port.endpoint_result;
}
static HRESULT port_get(ma_IMMDeviceEnumerator *value, const WCHAR *id,
                        ma_IMMDevice **out)
{
    TEST_ASSERT_EQUAL_PTR(&enumerator, value);
    ++port.gets; port.device_id = id;
    *out = FAILED(port.endpoint_result) ? NULL : &device;
    return port.endpoint_result;
}
static ULONG port_release(ma_IMMDeviceEnumerator *value)
{
    TEST_ASSERT_EQUAL_PTR(&enumerator, value);
    ++port.releases;
    return 0;
}
static ma_log *port_log(ma_context *ctx)
{
    TEST_ASSERT_EQUAL_PTR(&context, ctx);
    return NULL;
}
static ma_result port_log_message(ma_log *log, ma_uint32 level,
                                  const char *format, ...)
{
    (void)log; (void)level;
    TEST_ASSERT_NOT_NULL(format);
    ++port.logs;
    return MA_SUCCESS;
}
static ma_result port_result(HRESULT hr)
{
    TEST_ASSERT_TRUE(FAILED(hr));
    return MA_ERROR;
}

#ifdef MA_ASSERT
#undef MA_ASSERT
#endif
#define MA_ASSERT(value) TEST_ASSERT_TRUE(value)
#define ma_CoInitializeEx port_initialize
#define ma_CoUninitialize port_uninitialize
#define ma_CoCreateInstance port_create
#define ma_IMMDeviceEnumerator_GetDefaultAudioEndpoint port_default
#define ma_IMMDeviceEnumerator_GetDevice port_get
#define ma_IMMDeviceEnumerator_Release port_release
#define ma_context_get_log port_log
#define ma_log_postf port_log_message
#define ma_result_from_HRESULT port_result
#include "selected_miniaudio_com.inc.h"

void setUp(void)
{
    memset(&port, 0, sizeof port);
    port.initialize_result = port.create_result = port.endpoint_result = S_OK;
    port.references = 1; /* Pre-existing SDL STA reference belongs to its owner. */
}
void tearDown(void) {}

static void run_boundary(HRESULT initialized, HRESULT created, HRESULT endpoint,
                         bool explicit_id, bool capture)
{
    ma_device_id id;
    ma_IMMDevice *actual = NULL;
    memset(&id, 0, sizeof id);
    id.wasapi[0] = L'x';
    port.initialize_result = initialized;
    port.create_result = created;
    port.endpoint_result = endpoint;
    ma_result result = ma_context_get_MMDevice__wasapi(&context,
        capture ? ma_device_type_capture : ma_device_type_playback,
        explicit_id ? &id : NULL, &actual);
    TEST_ASSERT_EQUAL_UINT(1, port.initializes);
    TEST_ASSERT_EQUAL_UINT(1, port.creates);
    TEST_ASSERT_EQUAL_UINT(MA_COINIT_VALUE, port.coinit_flags);
    TEST_ASSERT_EQUAL_UINT(initialized == S_OK || initialized == S_FALSE,
                           port.uninitializes);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, port.references,
        "miniaudio released a COM reference owned by SDL or retained its own reference");
    TEST_ASSERT_EQUAL_INT(FAILED(created) || FAILED(endpoint) ? MA_ERROR : MA_SUCCESS,
                          result);
    TEST_ASSERT_EQUAL_PTR(FAILED(created) || FAILED(endpoint) ? NULL : &device, actual);
    TEST_ASSERT_EQUAL_UINT(FAILED(created) ? 0 : 1, port.releases);
    TEST_ASSERT_EQUAL_UINT(!FAILED(created) && !explicit_id, port.endpoints);
    TEST_ASSERT_EQUAL_UINT(!FAILED(created) && explicit_id, port.gets);
    TEST_ASSERT_EQUAL_UINT(FAILED(created) || FAILED(endpoint), port.logs);
    if (!FAILED(created)) {
        if (explicit_id) TEST_ASSERT_EQUAL_PTR(id.wasapi, port.device_id);
        else TEST_ASSERT_EQUAL_INT(capture ? ma_eCapture : ma_eRender, port.flow);
    }
}
static void test_s_ok_balances_exactly_one_render_reference(void)
{ run_boundary(S_OK, S_OK, S_OK, false, false); }
static void test_s_false_balances_exactly_one_capture_reference(void)
{ run_boundary(S_FALSE, S_OK, S_OK, false, true); }
static void test_changed_mode_preserves_sdl_reference(void)
{ run_boundary(RPC_E_CHANGED_MODE, S_OK, S_OK, false, false); }
static void test_failed_init_preserves_sdl_reference(void)
{ run_boundary(E_FAIL, S_OK, S_OK, false, false); }
static void test_out_of_memory_init_preserves_sdl_reference(void)
{ run_boundary(E_OUTOFMEMORY, S_OK, S_OK, false, true); }
static void test_changed_mode_create_failure_preserves_sdl_reference(void)
{ run_boundary(RPC_E_CHANGED_MODE, E_FAIL, S_OK, false, false); }
static void test_failed_init_and_create_do_not_uninitialize(void)
{ run_boundary(E_FAIL, E_OUTOFMEMORY, S_OK, false, false); }
static void test_s_ok_create_failure_still_balances_own_reference(void)
{ run_boundary(S_OK, E_FAIL, S_OK, false, false); }
static void test_s_false_create_failure_still_balances_own_reference(void)
{ run_boundary(S_FALSE, E_FAIL, S_OK, false, false); }
static void test_endpoint_failure_releases_enumerator_and_own_reference(void)
{ run_boundary(S_OK, S_OK, E_FAIL, false, false); }
static void test_changed_mode_endpoint_failure_does_not_release_sdl(void)
{ run_boundary(RPC_E_CHANGED_MODE, S_OK, E_FAIL, false, true); }
static void test_explicit_device_id_and_s_false_keep_ownership(void)
{ run_boundary(S_FALSE, S_OK, S_OK, true, false); }
static void test_explicit_device_failure_and_changed_mode_keep_sdl(void)
{ run_boundary(RPC_E_CHANGED_MODE, S_OK, E_FAIL, true, false); }

int main(void)
{
    printf("selected_miniaudio_header=%s sha256=%s function_sha256=%s\n",
        JCE_TEST_MINIAUDIO_HEADER_PATH, JCE_TEST_MINIAUDIO_HEADER_SHA256,
        JCE_TEST_MINIAUDIO_FUNCTION_SHA256);
    UNITY_BEGIN();
    RUN_TEST(test_s_ok_balances_exactly_one_render_reference);
    RUN_TEST(test_s_false_balances_exactly_one_capture_reference);
    RUN_TEST(test_changed_mode_preserves_sdl_reference);
    RUN_TEST(test_failed_init_preserves_sdl_reference);
    RUN_TEST(test_out_of_memory_init_preserves_sdl_reference);
    RUN_TEST(test_changed_mode_create_failure_preserves_sdl_reference);
    RUN_TEST(test_failed_init_and_create_do_not_uninitialize);
    RUN_TEST(test_s_ok_create_failure_still_balances_own_reference);
    RUN_TEST(test_s_false_create_failure_still_balances_own_reference);
    RUN_TEST(test_endpoint_failure_releases_enumerator_and_own_reference);
    RUN_TEST(test_changed_mode_endpoint_failure_does_not_release_sdl);
    RUN_TEST(test_explicit_device_id_and_s_false_keep_ownership);
    RUN_TEST(test_explicit_device_failure_and_changed_mode_keep_sdl);
    return UNITY_END();
}
