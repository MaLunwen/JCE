#include <windows.h>

static UINT mock_system_directory(wchar_t*, UINT);
static HMODULE mock_load(const wchar_t*);
static FARPROC mock_symbol(HMODULE, const char*);
static BOOL mock_free(HMODULE);

#define GetSystemDirectoryW mock_system_directory
#define LoadLibraryW mock_load
#define GetProcAddress mock_symbol
#define FreeLibrary mock_free
#include "../../../cmake/toolchains/windows-xp-bgfx-vulkan.cpp"
#undef GetSystemDirectoryW
#undef LoadLibraryW
#undef GetProcAddress
#undef FreeLibrary

#include <cassert>
#include <cwchar>

enum class Case { NoLoader, NoWsi, NoDevice, CpuOnly, NoSwapchain, NoPresentation, NoDestroy, Hardware };
static Case active_case;
static int opened, freed, created, destroyed;
static int module_token, instance_token, device_token;
static constexpr wchar_t system_directory[] = L"C:\\Windows\\System32";

static UINT mock_system_directory(wchar_t* output, UINT capacity)
{
    assert(capacity >= sizeof(system_directory) / sizeof(system_directory[0]));
    std::wcscpy(output, system_directory);
    return static_cast<UINT>(std::wcslen(system_directory));
}

static HMODULE mock_load(const wchar_t* path)
{
    assert(std::wcscmp(path, L"C:\\Windows\\System32\\vulkan-1.dll") == 0);
    if (active_case == Case::NoLoader)
        return nullptr;
    ++opened;
    return reinterpret_cast<HMODULE>(&module_token);
}

static BOOL mock_free(HMODULE module)
{
    assert(module == reinterpret_cast<HMODULE>(&module_token));
    assert(created == destroyed);
    ++freed;
    return TRUE;
}

static VkResult write_extensions(uint32_t* count, VkExtensionProperties* properties,
                                 const char* first, const char* second)
{
    const uint32_t available = second ? 2 : 1;
    if (properties) {
        assert(*count >= available);
        std::strcpy(properties[0].extensionName, first);
        if (second)
            std::strcpy(properties[1].extensionName, second);
    }
    *count = available;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL instance_extensions(const char*, uint32_t* count,
                                               VkExtensionProperties* properties)
{
    return write_extensions(count, properties, VK_KHR_SURFACE_EXTENSION_NAME,
                            active_case == Case::NoWsi ? nullptr : VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
}

static VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo* info,
                                           const VkAllocationCallbacks*, VkInstance* instance)
{
    assert(info->enabledExtensionCount == 2);
    assert(std::strcmp(info->ppEnabledExtensionNames[0], VK_KHR_SURFACE_EXTENSION_NAME) == 0);
    assert(std::strcmp(info->ppEnabledExtensionNames[1], VK_KHR_WIN32_SURFACE_EXTENSION_NAME) == 0);
    ++created;
    *instance = reinterpret_cast<VkInstance>(&instance_token);
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_instance(VkInstance instance, const VkAllocationCallbacks*)
{
    assert(instance == reinterpret_cast<VkInstance>(&instance_token));
    ++destroyed;
}

static VkResult VKAPI_CALL devices(VkInstance instance, uint32_t* count, VkPhysicalDevice* output)
{
    assert(instance == reinterpret_cast<VkInstance>(&instance_token));
    *count = active_case == Case::NoDevice ? 0 : 1;
    if (*count != 0 && output)
        output[0] = reinterpret_cast<VkPhysicalDevice>(&device_token);
    return VK_SUCCESS;
}

static void VKAPI_CALL device_properties(VkPhysicalDevice, VkPhysicalDeviceProperties* output)
{
    output->deviceType = active_case == Case::CpuOnly
        ? VK_PHYSICAL_DEVICE_TYPE_CPU : VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
}

static VkResult VKAPI_CALL device_extensions(VkPhysicalDevice, const char*, uint32_t* count,
                                             VkExtensionProperties* properties)
{
    return write_extensions(count, properties, VK_KHR_MAINTENANCE1_EXTENSION_NAME,
                            active_case == Case::NoSwapchain ? nullptr : VK_KHR_SWAPCHAIN_EXTENSION_NAME);
}

static void VKAPI_CALL queue_families(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* output)
{
    *count = 1;
    if (output) {
        output[0].queueCount = 1;
        output[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    }
}

static VkBool32 VKAPI_CALL supports_presentation(VkPhysicalDevice, uint32_t queue)
{
    assert(queue == 0);
    return active_case == Case::NoPresentation ? VK_FALSE : VK_TRUE;
}

static void VKAPI_CALL unused_surface_proc()
{
    assert(false && "Preflight must not create a surface");
}

static PFN_vkVoidFunction VKAPI_CALL instance_symbol(VkInstance instance, const char* name)
{
    if (!instance) {
        if (std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(instance_extensions);
        if (std::strcmp(name, "vkCreateInstance") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(create_instance);
        return nullptr;
    }
    if (std::strcmp(name, "vkDestroyInstance") == 0 && active_case == Case::NoDestroy)
        return nullptr;
    const struct { const char* name; PFN_vkVoidFunction proc; } symbols[] = {
        { "vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(destroy_instance) },
        { "vkEnumeratePhysicalDevices", reinterpret_cast<PFN_vkVoidFunction>(devices) },
        { "vkGetPhysicalDeviceProperties", reinterpret_cast<PFN_vkVoidFunction>(device_properties) },
        { "vkEnumerateDeviceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(device_extensions) },
        { "vkGetPhysicalDeviceQueueFamilyProperties", reinterpret_cast<PFN_vkVoidFunction>(queue_families) },
        { "vkGetPhysicalDeviceWin32PresentationSupportKHR", reinterpret_cast<PFN_vkVoidFunction>(supports_presentation) },
        { "vkCreateWin32SurfaceKHR", unused_surface_proc },
        { "vkGetPhysicalDeviceSurfaceSupportKHR", unused_surface_proc },
    };
    for (const auto& symbol : symbols) {
        if (std::strcmp(name, symbol.name) == 0)
            return symbol.proc;
    }
    return nullptr;
}

static FARPROC mock_symbol(HMODULE module, const char* name)
{
    assert(module == reinterpret_cast<HMODULE>(&module_token));
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<FARPROC>(instance_symbol);
    if (std::strcmp(name, "vkDestroyInstance") == 0)
        return reinterpret_cast<FARPROC>(destroy_instance);
    return nullptr;
}

int main()
{
    const struct { Case mode; int accepted, modules, instances; } cases[] = {
        { Case::NoLoader, 0, 0, 0 },
        { Case::NoWsi, 0, 1, 0 },
        { Case::NoDevice, 0, 1, 1 },
        { Case::CpuOnly, 0, 1, 1 },
        { Case::NoSwapchain, 0, 1, 1 },
        { Case::NoPresentation, 0, 1, 1 },
        { Case::NoDestroy, 0, 1, 1 },
        { Case::Hardware, 1, 1, 1 },
    };
    for (const auto& fixture : cases) {
        active_case = fixture.mode;
        opened = freed = created = destroyed = 0;
        assert(jce_xp_bgfx_vulkan_probe() == fixture.accepted);
        assert(opened == fixture.modules && freed == fixture.modules);
        assert(created == fixture.instances && destroyed == fixture.instances);
    }
}
