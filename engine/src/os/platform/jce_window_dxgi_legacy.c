/* XP-built binaries can use newer Direct3D providers when the host qualifies. */
#include "jce_window_adapter_internal.h"

#if JCE_PLATFORM_WINDOWS && defined(JCE_WINXP_LEGACY) && JCE_WINXP_LEGACY
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#include <wchar.h>
#include <string.h>
#include <jce/os/platform/jce_library.h>

typedef UINT (WINAPI *JceSystemDirectory)(LPWSTR, UINT);
typedef HRESULT (WINAPI *JceCreateLegacyDxgiFactory)(REFIID, void **);

static JceLibrary legacy_system_library(const wchar_t *directory, size_t length,
                                        const wchar_t *name)
{
    wchar_t path[MAX_PATH + 16];
    char utf8[4 * (MAX_PATH + 16)];
    const size_t name_length = wcslen(name);
    if (length + 1 + name_length >= sizeof path / sizeof path[0]) return NULL;
    memcpy(path, directory, length * sizeof path[0]);
    path[length++] = L'\\';
    memcpy(path + length, name, (name_length + 1) * sizeof path[0]);
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof utf8, NULL, NULL))
        return NULL;
    return jce_library_open(utf8);
}

static UINT legacy_system_directory(wchar_t *system, UINT capacity,
                                    bool require_create_event_ex)
{
    JceLibrary kernel = jce_library_open("kernel32.dll");
    const JceSystemDirectory system_directory =
        (JceSystemDirectory)jce_library_symbol(kernel, "GetSystemDirectoryW");
    const bool event_api = !require_create_event_ex ||
        jce_library_symbol(kernel, "CreateEventExA");
    const UINT length = system_directory && event_api
        ? system_directory(system, capacity) : 0;
    jce_library_close(kernel);
    return length < capacity ? length : 0;
}

static bool legacy_dxgi_factory_available(const wchar_t *system, UINT length,
                                           bool require_factory4)
{
    static const GUID factory_iid =
        {0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};
    static const GUID factory2_iid =
        {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
    static const GUID factory4_iid =
        {0x1bc6ea02, 0xef36, 0x464f, {0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a}};

    JceLibrary dxgi = legacy_system_library(system, length, L"dxgi.dll");
    JceCreateLegacyDxgiFactory create =
        (JceCreateLegacyDxgiFactory)jce_library_symbol(dxgi, "CreateDXGIFactory1");
    if (!create)
        create = (JceCreateLegacyDxgiFactory)jce_library_symbol(dxgi, "CreateDXGIFactory");
    IUnknown *factory = NULL, *factory2 = NULL, *factory4 = NULL;
    bool available = false;
    if (create && SUCCEEDED(create(&factory_iid, (void **)&factory)) && factory) {
        /* Pinned bgfx obtains the classic IID and calls Factory2 methods on
         * that original pointer. Refuse a distinct COM interface pointer. */
        available = SUCCEEDED(factory->lpVtbl->QueryInterface(factory,
            &factory2_iid, (void **)&factory2)) && factory2 == factory;
        /* D3D12's pinned present/resize path requires SwapChain3 methods.
         * Factory4 qualifies the system's DXGI 1.4 API before device init. */
        if (available && require_factory4)
            available = SUCCEEDED(factory->lpVtbl->QueryInterface(factory,
                &factory4_iid, (void **)&factory4)) && factory4;
    }
    if (factory4) factory4->lpVtbl->Release(factory4);
    if (factory2) factory2->lpVtbl->Release(factory2);
    if (factory) factory->lpVtbl->Release(factory);
    jce_library_close(dxgi);
    return available;
}

bool jce_window_legacy_d3d11_available(void)
{
    wchar_t system[MAX_PATH];
    const UINT length = legacy_system_directory(system, MAX_PATH, false);
    if (!length) return false;

    JceLibrary d3d11 = legacy_system_library(system, length, L"d3d11.dll");
    const bool available = jce_library_symbol(d3d11, "D3D11CreateDevice") != NULL;
    jce_library_close(d3d11);
    return available && legacy_dxgi_factory_available(system, length, false);
}

bool jce_window_legacy_d3d12_available(void)
{
    wchar_t system[MAX_PATH];
    const UINT length = legacy_system_directory(system, MAX_PATH, true);
    if (!length) return false;

    JceLibrary d3d12 = legacy_system_library(system, length, L"d3d12.dll");
    const bool available = jce_library_symbol(d3d12, "D3D12CreateDevice") &&
        jce_library_symbol(d3d12, "D3D12GetDebugInterface") &&
        jce_library_symbol(d3d12, "D3D12SerializeRootSignature");
    jce_library_close(d3d12);
    return available && legacy_dxgi_factory_available(system, length, true);
}

bool jce_window_legacy_d3d11_is_software(void *native_device)
{
    static const GUID device_iid =
        {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
    static const GUID adapter1_iid =
        {0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};
    IUnknown *device = (IUnknown *)native_device;
    if (!device) return false;

    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIAdapter1 *adapter1 = NULL;
    DXGI_ADAPTER_DESC1 desc = {0};
    /* The selected D3D device, not the provider's pre-init adapter guess,
     * identifies WARP when bgfx internally retries hardware creation. */
    const bool software = SUCCEEDED(IUnknown_QueryInterface(device, &device_iid,
        (void **)&dxgi_device)) && dxgi_device &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_device, &adapter)) && adapter &&
        SUCCEEDED(IDXGIAdapter_QueryInterface(adapter, &adapter1_iid,
            (void **)&adapter1)) && adapter1 &&
        SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter1, &desc)) &&
        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    if (adapter1) IDXGIAdapter1_Release(adapter1);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi_device) IDXGIDevice_Release(dxgi_device);
    return software;
}
#endif
