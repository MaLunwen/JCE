/* Exact XP provider preflight with counted loader and COM ports. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#include <jce/os/platform/jce_library.h>
#include "unity.h"
#include <string.h>

static const GUID classic_iid =
    {0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};
static const GUID factory2_iid =
    {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
static const GUID factory4_iid =
    {0x1bc6ea02, 0xef36, 0x464f, {0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a}};
static const GUID device_iid =
    {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const GUID adapter1_iid =
    {0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};

static char kernel, d3d11, d3d12, dxgi;
static IUnknown factory, other_factory, device;
static IDXGIDevice dxgi_device;
static IDXGIAdapter adapter;
static IDXGIAdapter1 adapter1;
static struct {
    unsigned opened, closed, creates, queries, factory_releases, device_releases;
    unsigned adapter_releases, adapter1_releases;
    bool no_system_directory, no_d3d11, no_device_export, no_factory1_export;
    bool no_dxgi, no_factory_export, no_factory2, distinct_factory2;
    bool no_d3d12, no_event_export, no_factory4;
    const char *no_d3d12_export;
    bool no_adapter1, software;
} port;

static UINT WINAPI fixture_system_directory(LPWSTR out, UINT capacity)
{
    static const wchar_t path[] = L"C:\\OS\\System32";
    TEST_ASSERT_TRUE(capacity >= sizeof path / sizeof path[0]);
    if (port.no_system_directory) return 0;
    memcpy(out, path, sizeof path);
    return sizeof path / sizeof path[0] - 1;
}

static HRESULT WINAPI fixture_create_factory(REFIID iid, void **out)
{
    TEST_ASSERT_EQUAL_MEMORY(&classic_iid, iid, sizeof classic_iid);
    ++port.creates;
    *out = &factory;
    return S_OK;
}

static HRESULT WINAPI fixture_factory_query(IUnknown *self, REFIID iid, void **out)
{
    TEST_ASSERT_EQUAL_PTR(&factory, self);
    ++port.queries;
    if (memcmp(&factory2_iid, iid, sizeof factory2_iid) == 0) {
        if (port.no_factory2) return E_NOINTERFACE;
        *out = port.distinct_factory2 ? &other_factory : &factory;
    } else {
        TEST_ASSERT_EQUAL_MEMORY(&factory4_iid, iid, sizeof factory4_iid);
        if (port.no_factory4) return E_NOINTERFACE;
        *out = &other_factory;
    }
    return S_OK;
}

static ULONG WINAPI fixture_factory_release(IUnknown *self)
{
    TEST_ASSERT_TRUE(self == &factory || self == &other_factory);
    return ++port.factory_releases;
}

static HRESULT WINAPI fixture_device_query(IUnknown *self, REFIID iid, void **out)
{
    TEST_ASSERT_EQUAL_PTR(&device, self);
    TEST_ASSERT_EQUAL_MEMORY(&device_iid, iid, sizeof device_iid);
    *out = &dxgi_device;
    return S_OK;
}

static HRESULT WINAPI fixture_get_adapter(IDXGIDevice *self, IDXGIAdapter **out)
{
    TEST_ASSERT_EQUAL_PTR(&dxgi_device, self);
    *out = &adapter;
    return S_OK;
}

static HRESULT WINAPI fixture_adapter_query(IDXGIAdapter *self, REFIID iid, void **out)
{
    TEST_ASSERT_EQUAL_PTR(&adapter, self);
    TEST_ASSERT_EQUAL_MEMORY(&adapter1_iid, iid, sizeof adapter1_iid);
    if (port.no_adapter1) return E_NOINTERFACE;
    *out = &adapter1;
    return S_OK;
}

static HRESULT WINAPI fixture_adapter_desc(IDXGIAdapter1 *self, DXGI_ADAPTER_DESC1 *out)
{
    TEST_ASSERT_EQUAL_PTR(&adapter1, self);
    out->Flags = port.software ? DXGI_ADAPTER_FLAG_SOFTWARE : 0;
    return S_OK;
}

static ULONG WINAPI fixture_device_release(IDXGIDevice *self)
{
    TEST_ASSERT_EQUAL_PTR(&dxgi_device, self);
    return ++port.device_releases;
}

static ULONG WINAPI fixture_adapter_release(IDXGIAdapter *self)
{
    TEST_ASSERT_EQUAL_PTR(&adapter, self);
    return ++port.adapter_releases;
}

static ULONG WINAPI fixture_adapter1_release(IDXGIAdapter1 *self)
{
    TEST_ASSERT_EQUAL_PTR(&adapter1, self);
    return ++port.adapter1_releases;
}

static JceLibrary fixture_open(const char *path)
{
    JceLibrary handle = NULL;
    if (strcmp(path, "kernel32.dll") == 0) handle = &kernel;
    else if (strcmp(path, "C:\\OS\\System32\\d3d11.dll") == 0) {
        if (!port.no_d3d11) handle = &d3d11;
    } else if (strcmp(path, "C:\\OS\\System32\\d3d12.dll") == 0) {
        if (!port.no_d3d12) handle = &d3d12;
    } else if (strcmp(path, "C:\\OS\\System32\\dxgi.dll") == 0) {
        if (!port.no_dxgi) handle = &dxgi;
    } else TEST_FAIL_MESSAGE("non-system renderer DLL path");
    if (handle) ++port.opened;
    return handle;
}

static void *fixture_symbol(JceLibrary handle, const char *symbol)
{
    if (!handle) return NULL;
    if (handle == &kernel && strcmp(symbol, "GetSystemDirectoryW") == 0)
        return (void *)fixture_system_directory;
    if (handle == &kernel && strcmp(symbol, "CreateEventExA") == 0)
        return port.no_event_export ? NULL : &kernel;
    if (handle == &d3d11 && strcmp(symbol, "D3D11CreateDevice") == 0)
        return port.no_device_export ? NULL : &d3d11; /* Presence is sufficient. */
    if (handle == &d3d12 &&
        (strcmp(symbol, "D3D12CreateDevice") == 0 ||
         strcmp(symbol, "D3D12GetDebugInterface") == 0 ||
         strcmp(symbol, "D3D12SerializeRootSignature") == 0))
        return port.no_d3d12_export && strcmp(symbol, port.no_d3d12_export) == 0
            ? NULL : &d3d12;
    if (handle == &dxgi && strcmp(symbol, "CreateDXGIFactory1") == 0)
        return port.no_factory1_export ? NULL : (void *)fixture_create_factory;
    if (handle == &dxgi && strcmp(symbol, "CreateDXGIFactory") == 0)
        return port.no_factory_export ? NULL : (void *)fixture_create_factory;
    TEST_FAIL_MESSAGE("unexpected renderer DLL symbol");
    return NULL;
}

static void fixture_close(JceLibrary handle)
{
    if (handle) ++port.closed;
}

#define JCE_WINXP_LEGACY 1
#define jce_library_open fixture_open
#define jce_library_symbol fixture_symbol
#define jce_library_close fixture_close
#include "os/platform/jce_window_dxgi_legacy.c"
#undef jce_library_close
#undef jce_library_symbol
#undef jce_library_open
#undef JCE_WINXP_LEGACY

void setUp(void)
{
    static IUnknownVtbl factory_vtable = {
        .QueryInterface = fixture_factory_query, .Release = fixture_factory_release
    };
    static IUnknownVtbl device_vtable = { .QueryInterface = fixture_device_query };
    static IDXGIDeviceVtbl dxgi_device_vtable = {
        .GetAdapter = fixture_get_adapter, .Release = fixture_device_release
    };
    static IDXGIAdapterVtbl adapter_vtable = {
        .QueryInterface = fixture_adapter_query, .Release = fixture_adapter_release
    };
    static IDXGIAdapter1Vtbl adapter1_vtable = {
        .GetDesc1 = fixture_adapter_desc, .Release = fixture_adapter1_release
    };
    memset(&port, 0, sizeof port);
    factory.lpVtbl = other_factory.lpVtbl = &factory_vtable;
    device.lpVtbl = &device_vtable;
    dxgi_device.lpVtbl = &dxgi_device_vtable;
    adapter.lpVtbl = &adapter_vtable;
    adapter1.lpVtbl = &adapter1_vtable;
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT(port.opened, port.closed);
}

static void actual_system_preflight_requires_dll_and_device_export(void)
{
    port.no_system_directory = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d11_available());
    port.no_system_directory = false;
    port.no_d3d11 = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d11_available());
    port.no_d3d11 = false;
    port.no_device_export = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d11_available());
    port.no_device_export = false;
    port.no_dxgi = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d11_available());
    TEST_ASSERT_EQUAL_UINT(0, port.creates);
}

static void actual_factory_preflight_requires_the_same_factory2_pointer(void)
{
    port.no_factory1_export = port.no_factory_export = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d11_available());
    TEST_ASSERT_EQUAL_UINT(0, port.creates);
    port.no_factory_export = false;
    port.no_factory2 = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d11_available());
    TEST_ASSERT_EQUAL_UINT(1, port.factory_releases);
    port.no_factory2 = false;
    port.distinct_factory2 = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d11_available());
    TEST_ASSERT_EQUAL_UINT(3, port.factory_releases);
    port.distinct_factory2 = false;
    port.no_factory1_export = false;
    TEST_ASSERT_TRUE(jce_window_legacy_d3d11_available());
    TEST_ASSERT_EQUAL_UINT(5, port.factory_releases);
    TEST_ASSERT_EQUAL_UINT(3, port.creates);
    TEST_ASSERT_EQUAL_UINT(3, port.queries);
}

static void actual_device_flags_distinguish_software_and_unavailable(void)
{
    TEST_ASSERT_FALSE(jce_window_legacy_d3d11_is_software(NULL));
    TEST_ASSERT_FALSE(jce_window_legacy_d3d11_is_software(&device));
    port.software = true;
    TEST_ASSERT_TRUE(jce_window_legacy_d3d11_is_software(&device));
    port.no_adapter1 = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d11_is_software(&device));
    TEST_ASSERT_EQUAL_UINT(3, port.device_releases);
    TEST_ASSERT_EQUAL_UINT(3, port.adapter_releases);
    TEST_ASSERT_EQUAL_UINT(2, port.adapter1_releases);
}

static void d3d12_requires_its_system_exports_and_does_not_probe_dxgi_early(void)
{
    static const char *exports[] = {
        "D3D12CreateDevice", "D3D12GetDebugInterface", "D3D12SerializeRootSignature"
    };
    port.no_event_export = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d12_available());
    port.no_event_export = false;
    port.no_d3d12 = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d12_available());
    port.no_d3d12 = false;
    for (unsigned i = 0; i < sizeof exports / sizeof exports[0]; ++i) {
        port.no_d3d12_export = exports[i];
        TEST_ASSERT_FALSE(jce_window_legacy_d3d12_available());
    }
    TEST_ASSERT_EQUAL_UINT(0, port.creates);
}

static void d3d12_requires_factory4_in_addition_to_the_exact_factory2_abi(void)
{
    port.no_factory4 = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d12_available());
    TEST_ASSERT_EQUAL_UINT(2, port.factory_releases);
    port.no_event_export = true;
    TEST_ASSERT_TRUE(jce_window_legacy_d3d11_available());
    TEST_ASSERT_EQUAL_UINT(4, port.factory_releases);
    port.no_event_export = false;
    port.no_factory4 = false;
    port.distinct_factory2 = true;
    TEST_ASSERT_FALSE(jce_window_legacy_d3d12_available());
    TEST_ASSERT_EQUAL_UINT(6, port.factory_releases);
    port.distinct_factory2 = false;
    TEST_ASSERT_TRUE(jce_window_legacy_d3d12_available());
    TEST_ASSERT_EQUAL_UINT(9, port.factory_releases);
    TEST_ASSERT_EQUAL_UINT(6, port.queries);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(actual_system_preflight_requires_dll_and_device_export);
    RUN_TEST(actual_factory_preflight_requires_the_same_factory2_pointer);
    RUN_TEST(actual_device_flags_distinguish_software_and_unavailable);
    RUN_TEST(d3d12_requires_its_system_exports_and_does_not_probe_dxgi_early);
    RUN_TEST(d3d12_requires_factory4_in_addition_to_the_exact_factory2_abi);
    return UNITY_END();
}
