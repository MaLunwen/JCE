#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan-local/vulkan.h>

#include <cstring>
#include <memory>
#include <new>

namespace {

template<typename Function>
Function instance_proc(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance,
                       const char* name)
{
    return reinterpret_cast<Function>(get_proc(instance, name));
}

template<typename Enumerate, size_t Size>
bool has_extensions(Enumerate enumerate, const char* const (&required)[Size])
{
    uint32_t count = 0;
    if (enumerate(&count, nullptr) != VK_SUCCESS || count == 0 || count > 1024)
        return false;

    const uint32_t capacity = count;
    std::unique_ptr<VkExtensionProperties[]> properties(
        new (std::nothrow) VkExtensionProperties[capacity]);
    if (!properties || enumerate(&count, properties.get()) != VK_SUCCESS || count > capacity)
        return false;

    for (const char* name : required) {
        uint32_t i = 0;
        while (i < count && std::strcmp(name, properties[i].extensionName) != 0)
            ++i;
        if (i == count)
            return false;
    }
    return true;
}

bool device_has_presentation_queue(VkPhysicalDevice device,
                                   PFN_vkGetPhysicalDeviceQueueFamilyProperties queues,
                                   PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR present)
{
    VkQueueFamilyProperties properties[64];
    uint32_t count = 0;
    queues(device, &count, nullptr);
    if (count == 0 || count > 64)
        return false;
    queues(device, &count, properties);
    if (count > 64)
        return false;

    // The pinned renderer picks the first combined graphics and compute queue.
    constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32_t i = 0; i < count; ++i) {
        if ((properties[i].queueFlags & required) == required)
            return properties[i].queueCount != 0 && present(device, i) == VK_TRUE;
    }
    return false;
}

bool has_device(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance)
{
    const auto enumerate = instance_proc<PFN_vkEnumeratePhysicalDevices>(
        get_proc, instance, "vkEnumeratePhysicalDevices");
    const auto properties = instance_proc<PFN_vkGetPhysicalDeviceProperties>(
        get_proc, instance, "vkGetPhysicalDeviceProperties");
    const auto extensions = instance_proc<PFN_vkEnumerateDeviceExtensionProperties>(
        get_proc, instance, "vkEnumerateDeviceExtensionProperties");
    const auto queues = instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        get_proc, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    const auto present = instance_proc<PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR>(
        get_proc, instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    if (!enumerate || !properties || !extensions || !queues || !present ||
        !get_proc(instance, "vkCreateWin32SurfaceKHR") ||
        !get_proc(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"))
        return false;

    // The pinned renderer accepts at most four enumerated devices.
    VkPhysicalDevice devices[4];
    uint32_t count = 4;
    if (enumerate(instance, &count, devices) != VK_SUCCESS || count > 4)
        return false;

    constexpr const char* required[] = {
        VK_KHR_MAINTENANCE1_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    for (uint32_t i = 0; i < count; ++i) {
        const VkPhysicalDevice device = devices[i];
        VkPhysicalDeviceProperties info{};
        properties(device, &info);
        if (info.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
            !device_has_presentation_queue(device, queues, present))
            continue;
        if (has_extensions([&](uint32_t* size, VkExtensionProperties* values) {
                return extensions(device, nullptr, size, values);
            }, required))
            return true;
    }
    return false;
}

bool probe_loader(HMODULE module)
{
    const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(module, "vkGetInstanceProcAddr"));
    const auto destroy_export = reinterpret_cast<PFN_vkDestroyInstance>(
        GetProcAddress(module, "vkDestroyInstance"));
    if (!get_proc || !destroy_export)
        return false;

    const auto enumerate = instance_proc<PFN_vkEnumerateInstanceExtensionProperties>(
        get_proc, VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    const auto create = instance_proc<PFN_vkCreateInstance>(
        get_proc, VK_NULL_HANDLE, "vkCreateInstance");
    constexpr const char* required[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    if (!enumerate || !create ||
        !has_extensions([&](uint32_t* size, VkExtensionProperties* values) {
            return enumerate(nullptr, size, values);
        }, required))
        return false;

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application;
    create_info.enabledExtensionCount = 2;
    create_info.ppEnabledExtensionNames = required;
    VkInstance instance = VK_NULL_HANDLE;
    if (create(&create_info, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE)
        return false;

    const auto destroy = instance_proc<PFN_vkDestroyInstance>(
        get_proc, instance, "vkDestroyInstance");
    const bool usable = destroy && has_device(get_proc, instance);
    (destroy ? destroy : destroy_export)(instance, nullptr);
    return usable;
}

} // namespace

extern "C" int jce_xp_bgfx_vulkan_probe(void)
{
    constexpr wchar_t suffix[] = L"\\vulkan-1.dll";
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length > MAX_PATH - sizeof(suffix) / sizeof(suffix[0]))
        return 0;
    for (size_t i = 0; i < sizeof(suffix) / sizeof(suffix[0]); ++i)
        path[length + i] = suffix[i];

    const HMODULE module = LoadLibraryW(path);
    if (!module)
        return 0;
    const bool usable = probe_loader(module);
    FreeLibrary(module);
    return usable;
}
