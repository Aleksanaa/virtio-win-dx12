// SPDX-License-Identifier: MIT
#include "internal.h"

#include <cstdio>
#include <cstring>

namespace vgd {

namespace {

LogSink g_sink = nullptr;

bool has_extension(const std::vector<VkExtensionProperties>& exts, const char* name)
{
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0)
            return true;
    return false;
}

} // namespace

void set_log_sink(LogSink sink) { g_sink = sink; }

void log(LogLevel level, const char* fmt, ...)
{
    if (!g_sink)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_sink(level, buf);
}

HRESULT hresult_from_vk(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return S_OK;
    case VK_TIMEOUT:
    case VK_NOT_READY: return S_FALSE;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return E_OUTOFMEMORY;
    case VK_ERROR_DEVICE_LOST: return DXGI_ERROR_DEVICE_REMOVED;
    default: return E_FAIL;
    }
}

Instance::Impl::~Impl()
{
    if (instance)
        vk.vkDestroyInstance(instance, nullptr);
}

Instance::Instance(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Instance::~Instance() = default;

const std::vector<AdapterDesc>& Instance::adapters() const { return impl_->adapters; }

static void probe_physical_device(Instance::Impl& in, VkPhysicalDevice pd)
{
    PhysicalDevice phys;
    phys.handle = pd;
    auto& d = phys.desc;

    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    in.vk.vkGetPhysicalDeviceProperties2(pd, &props);
    phys.limits = props.properties.limits;
    d.name = props.properties.deviceName;
    d.vendor_id = props.properties.vendorID;
    d.device_id = props.properties.deviceID;
    d.api_version = props.properties.apiVersion;
    d.driver_info = driver.driverInfo;

    uint32_t count = 0;
    in.vk.vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    in.vk.vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, exts.data());

    const bool is_1_4 = d.api_version >= VK_API_VERSION_1_4;
    if (d.api_version < VK_API_VERSION_1_3)
        d.missing.push_back("Vulkan 1.3");
    if (!is_1_4) {
        if (has_extension(exts, VK_KHR_MAINTENANCE_5_EXTENSION_NAME))
            phys.needs_maintenance5_ext = true;
        else
            d.missing.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    }
    const bool has_heap_ext = has_extension(exts, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);

    if (d.api_version >= VK_API_VERSION_1_3) {
        VkPhysicalDeviceDescriptorHeapFeaturesEXT heap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
        VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
        VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
        if (is_1_4 || phys.needs_maintenance5_ext) {
            m5.pNext = f2.pNext;
            f2.pNext = &m5;
        }
        if (has_heap_ext) {
            heap.pNext = f2.pNext;
            f2.pNext = &heap;
        }
        in.vk.vkGetPhysicalDeviceFeatures2(pd, &f2);
        phys.features = f2.features;
        phys.features.robustBufferAccess = VK_FALSE;
        phys.v11 = v11;
        phys.v12 = v12;
        phys.v13 = v13;
        phys.v13.robustImageAccess = VK_FALSE;   // like robustBufferAccess: a cost D3D12 does not ask for
        phys.v11.protectedMemory = VK_FALSE;   // and features with no D3D12 use that only add risk
        phys.v12.bufferDeviceAddressCaptureReplay = VK_FALSE;
        phys.v12.bufferDeviceAddressMultiDevice = VK_FALSE;

        if (!v12.timelineSemaphore) d.missing.push_back("timelineSemaphore");
        if (!v12.bufferDeviceAddress) d.missing.push_back("bufferDeviceAddress");
        if (!v13.synchronization2) d.missing.push_back("synchronization2");
        if (!v13.dynamicRendering) d.missing.push_back("dynamicRendering");
        if ((is_1_4 || phys.needs_maintenance5_ext) && !m5.maintenance5) d.missing.push_back("maintenance5");
        d.descriptor_heap = has_heap_ext && heap.descriptorHeap;
    }

    in.vk.vkGetPhysicalDeviceMemoryProperties(pd, &phys.memory);
    for (uint32_t i = 0; i < phys.memory.memoryHeapCount; i++) {
        const auto& heap = phys.memory.memoryHeaps[i];
        if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            d.device_local_bytes += heap.size;
        else
            d.host_visible_bytes += heap.size;
    }

    in.vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    phys.queue_families.resize(count);
    in.vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, phys.queue_families.data());

    in.adapters.push_back(d);
    in.physical_devices.push_back(std::move(phys));
}

HRESULT Instance::create(PFN_vkGetInstanceProcAddr gipa, std::unique_ptr<Instance>* out)
{
    if (!gipa || !out)
        return E_INVALIDARG;
    auto impl = std::make_unique<Impl>();
    auto& vk = impl->vk;

#define VGD_VK_GLOBAL(n) vk.n = reinterpret_cast<PFN_##n>(gipa(VK_NULL_HANDLE, #n));
#define VGD_VK_INSTANCE(n)
#define VGD_VK_DEVICE(n)
#include "vk_procs.inc"
#undef VGD_VK_GLOBAL
#undef VGD_VK_INSTANCE
#undef VGD_VK_DEVICE

    if (!vk.vkCreateInstance)
        return E_FAIL;
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vk.vkEnumerateInstanceVersion)
        vk.vkEnumerateInstanceVersion(&loader_version);
    if (loader_version < VK_API_VERSION_1_3) {
        log(LogLevel::error, "Vulkan instance version %u.%u is below 1.3",
            VK_API_VERSION_MAJOR(loader_version), VK_API_VERSION_MINOR(loader_version));
        return DXGI_ERROR_UNSUPPORTED;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vgd3d12";
    app.pEngineName = "vgd3d12";
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkResult r = vk.vkCreateInstance(&ci, nullptr, &impl->instance);
    if (r != VK_SUCCESS) {
        log(LogLevel::error, "vkCreateInstance failed: %d", r);
        return hresult_from_vk(r);
    }

#define VGD_VK_GLOBAL(n)
#define VGD_VK_INSTANCE(n) vk.n = reinterpret_cast<PFN_##n>(gipa(impl->instance, #n));
#define VGD_VK_DEVICE(n)
#include "vk_procs.inc"
#undef VGD_VK_GLOBAL
#undef VGD_VK_INSTANCE
#undef VGD_VK_DEVICE

    uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(impl->instance, &count, nullptr);
    std::vector<VkPhysicalDevice> pds(count);
    vk.vkEnumeratePhysicalDevices(impl->instance, &count, pds.data());
    for (auto pd : pds)
        probe_physical_device(*impl, pd);

    out->reset(new Instance(std::move(impl)));
    return S_OK;
}

} // namespace vgd
