// SPDX-License-Identifier: MIT
#include "internal.h"

namespace vgd {

namespace {

// D3D12 queue types onto Vulkan families: DIRECT needs graphics; COMPUTE and COPY prefer dedicated
// families (async compute / DMA engines) and fall back to the graphics family.
struct FamilyChoice {
    uint32_t direct = UINT32_MAX, compute = UINT32_MAX, copy = UINT32_MAX;
};

FamilyChoice choose_families(const std::vector<VkQueueFamilyProperties>& families)
{
    FamilyChoice c;
    for (uint32_t i = 0; i < families.size(); i++) {
        const VkQueueFlags f = families[i].queueFlags;
        const bool g = f & VK_QUEUE_GRAPHICS_BIT, cs = f & VK_QUEUE_COMPUTE_BIT, t = f & VK_QUEUE_TRANSFER_BIT;
        if (g && cs && c.direct == UINT32_MAX)
            c.direct = i;
        else if (!g && cs && c.compute == UINT32_MAX)
            c.compute = i;
        else if (!g && !cs && t && c.copy == UINT32_MAX)
            c.copy = i;
    }
    if (c.compute == UINT32_MAX)
        c.compute = c.direct;
    if (c.copy == UINT32_MAX)
        c.copy = c.compute;
    return c;
}

} // namespace

Device::Impl::~Impl()
{
    if (device) {
        vk.vkDeviceWaitIdle(device);
        for (auto& [key, s] : samplers)
            vk.vkDestroySampler(device, s, nullptr);
        if (resource_set_layout)
            vk.vkDestroyDescriptorSetLayout(device, resource_set_layout, nullptr);
        if (sampler_set_layout)
            vk.vkDestroyDescriptorSetLayout(device, sampler_set_layout, nullptr);
        if (empty_set_layout)
            vk.vkDestroyDescriptorSetLayout(device, empty_set_layout, nullptr);
        if (init_pool)
            vk.vkDestroyCommandPool(device, init_pool, nullptr);
        if (init_timeline)
            vk.vkDestroySemaphore(device, init_timeline, nullptr);
        vk.vkDestroyDevice(device, nullptr);
    }
}

QueueSlot& Device::Impl::slot_for(D3D12_COMMAND_LIST_TYPE type)
{
    switch (type) {
    case D3D12_COMMAND_LIST_TYPE_COMPUTE: return compute.family == direct.family ? direct : compute;
    case D3D12_COMMAND_LIST_TYPE_COPY:
        if (copy.family == direct.family) return direct;
        if (copy.family == compute.family) return compute;
        return copy;
    default: return direct;
    }
}

uint32_t Device::Impl::find_memory_type(uint32_t type_bits, D3D12_HEAP_TYPE heap_type) const
{
    // Ordered preferences per D3D12 heap type; the first matching memory type wins.
    constexpr VkMemoryPropertyFlags DL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr VkMemoryPropertyFlags HC = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    constexpr VkMemoryPropertyFlags CA = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    struct Pref { VkMemoryPropertyFlags required, avoided; };
    static constexpr Pref default_prefs[] = {{DL, HV}, {DL, 0}, {0, 0}};
    static constexpr Pref upload_prefs[] = {{HV | HC, CA | DL}, {HV | HC, CA}, {HV | HC, 0}};
    static constexpr Pref readback_prefs[] = {{HV | HC | CA, 0}, {HV | CA, 0}, {HV | HC, 0}};

    std::span<const Pref> prefs = default_prefs;
    if (heap_type == D3D12_HEAP_TYPE_UPLOAD)
        prefs = upload_prefs;
    else if (heap_type == D3D12_HEAP_TYPE_READBACK)
        prefs = readback_prefs;

    const auto& mem = physical->memory;
    for (const Pref& p : prefs)
        for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
            const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
            if ((type_bits & (1u << i)) && (f & p.required) == p.required && !(f & p.avoided))
                return i;
        }
    return UINT32_MAX;
}

HRESULT Instance::create_device(size_t adapter_index, std::unique_ptr<Device>* out)
{
    if (!out || adapter_index >= impl_->physical_devices.size())
        return E_INVALIDARG;
    const PhysicalDevice& phys = impl_->physical_devices[adapter_index];
    if (!phys.desc.usable())
        return DXGI_ERROR_UNSUPPORTED;

    auto impl = std::make_unique<Device::Impl>();
    impl->ivk = &impl_->vk;
    impl->physical = &phys;

    const FamilyChoice fam = choose_families(phys.queue_families);
    if (fam.direct == UINT32_MAX)
        return DXGI_ERROR_UNSUPPORTED;
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    for (uint32_t f : {fam.direct, fam.compute, fam.copy}) {
        bool seen = false;
        for (const auto& q : queue_infos)
            seen |= q.queueFamilyIndex == f;
        if (seen)
            continue;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = f;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        queue_infos.push_back(q);
    }

    std::vector<const char*> exts;
    if (phys.desc.descriptor_heap)
        exts.push_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
    if (phys.needs_maintenance5_ext)
        exts.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mut{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    mut.mutableDescriptorType = VK_TRUE;
    VkPhysicalDeviceRobustness2FeaturesEXT rob{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    rob.nullDescriptor = VK_TRUE;   // null descriptors only, not the robust access modes
    if (phys.mutable_descriptors)
        exts.push_back(phys.mutable_ext_name);
    if (phys.null_descriptor)
        exts.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

    VkPhysicalDeviceDescriptorHeapFeaturesEXT heap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    heap.descriptorHeap = VK_TRUE;
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
                                               phys.desc.descriptor_heap ? &heap : nullptr};
    m5.maintenance5 = VK_TRUE;
    // Every supported core feature is enabled; the ones we require were checked when the adapter was probed.
    // Optional extension features go first in the chain.
    void* chain = &m5;
    if (phys.mutable_descriptors) {
        mut.pNext = chain;
        chain = &mut;
    }
    if (phys.null_descriptor) {
        rob.pNext = chain;
        chain = &rob;
    }
    VkPhysicalDeviceVulkan13Features v13 = phys.v13;
    v13.pNext = chain;
    VkPhysicalDeviceVulkan12Features v12 = phys.v12;
    v12.pNext = &v13;
    VkPhysicalDeviceVulkan11Features v11 = phys.v11;
    v11.pNext = &v12;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
    f2.features = phys.features;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    ci.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    ci.pQueueCreateInfos = queue_infos.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    VkResult r = impl_->vk.vkCreateDevice(phys.handle, &ci, nullptr, &impl->device);
    if (r != VK_SUCCESS) {
        log(LogLevel::error, "vkCreateDevice failed: %d", r);
        return hresult_from_vk(r);
    }

    auto& vk = impl->vk;
    const auto gdpa = impl_->vk.vkGetDeviceProcAddr;
#define VGD_VK_GLOBAL(n)
#define VGD_VK_INSTANCE(n)
#define VGD_VK_DEVICE(n)                                                                                   \
    vk.n = reinterpret_cast<PFN_##n>(gdpa(impl->device, #n));                                             \
    if (!vk.n)                                                                                             \
        log(LogLevel::warn, "device entry point %s is missing", #n);
#include "vk_procs.inc"
#undef VGD_VK_GLOBAL
#undef VGD_VK_INSTANCE
#undef VGD_VK_DEVICE

    for (auto [slot, family] : {std::pair{&impl->direct, fam.direct}, {&impl->compute, fam.compute},
                                {&impl->copy, fam.copy}}) {
        slot->family = family;
        vk.vkGetDeviceQueue(impl->device, family, 0, &slot->queue);
    }
    for (const auto& q : queue_infos)
        impl->families.push_back(q.queueFamilyIndex);
    HRESULT hr = impl->create_init_objects();
    if (FAILED(hr))
        return hr;
    hr = impl->create_bindless_layouts();
    if (FAILED(hr))
        return hr;
    {
        VkFormatProperties2 d24{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        impl_->vk.vkGetPhysicalDeviceFormatProperties2(phys.handle, VK_FORMAT_D24_UNORM_S8_UINT, &d24);
        impl->d24s8_supported =
            d24.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    log(LogLevel::info, "device on %s: direct family %u, compute %u, copy %u, d24s8 %d", phys.desc.name.c_str(),
        fam.direct, fam.compute, fam.copy, impl->d24s8_supported);

    out->reset(new Device(std::move(impl)));
    return S_OK;
}

Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Device::~Device() = default;
const AdapterDesc& Device::adapter() const { return impl_->physical->desc; }

VkFormat Device::Impl::resolve_format(DXGI_FORMAT format) const
{
    const VkFormat vk = vk_format(format);
    if (vk == VK_FORMAT_D24_UNORM_S8_UINT && !d24s8_supported)
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    return vk;
}

FormatFeatures Device::format_features(DXGI_FORMAT format) const
{
    FormatFeatures out;
    const VkFormat vk = impl_->resolve_format(format);
    if (vk == VK_FORMAT_UNDEFINED)
        return out;
    VkFormatProperties3 p3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 p2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &p3};
    impl_->ivk->vkGetPhysicalDeviceFormatProperties2(impl_->physical->handle, vk, &p2);
    out.optimal = p3.optimalTilingFeatures;
    out.buffer = p3.bufferFeatures;

    VkImageUsageFlags usage = 0;
    if (out.optimal & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)
        usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    else if (out.optimal & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)
        usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    out.samples = VK_SAMPLE_COUNT_1_BIT;
    if (usage) {
        VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.format = vk;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        if (impl_->ivk->vkGetPhysicalDeviceImageFormatProperties2(impl_->physical->handle, &info, &props) == VK_SUCCESS)
            out.samples = props.imageFormatProperties.sampleCounts;
    }
    return out;
}

// --- Fences ------------------------------------------------------------------------------------------------

Fence::Impl::~Impl()
{
    if (timeline)
        device->vk.vkDestroySemaphore(device->device, timeline, nullptr);
}

Fence::Fence(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Fence::~Fence() = default;

HRESULT Device::create_fence(uint64_t initial_value, std::unique_ptr<Fence>* out)
{
    if (!out)
        return E_INVALIDARG;
    auto f = std::make_unique<Fence::Impl>();
    f->device = impl_.get();
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = initial_value;
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    VkResult r = impl_->vk.vkCreateSemaphore(impl_->device, &ci, nullptr, &f->timeline);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    out->reset(new Fence(std::move(f)));
    return S_OK;
}

uint64_t Fence::completed_value() const
{
    uint64_t v = 0;
    impl_->device->vk.vkGetSemaphoreCounterValue(impl_->device->device, impl_->timeline, &v);
    return v;
}

HRESULT Fence::signal(uint64_t value)
{
    // D3D12 allows a CPU signal to any value; Vulkan requires monotonic increase, so lower values are dropped.
    // TODO: D3D12 lets the fence value go backwards; supporting that needs a value shadow on top of the timeline.
    if (value <= completed_value())
        return S_OK;
    VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    si.semaphore = impl_->timeline;
    si.value = value;
    return hresult_from_vk(impl_->device->vk.vkSignalSemaphore(impl_->device->device, &si));
}

HRESULT Fence::wait(uint64_t value, uint64_t timeout_ns)
{
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &impl_->timeline;
    wi.pValues = &value;
    return hresult_from_vk(impl_->device->vk.vkWaitSemaphores(impl_->device->device, &wi, timeout_ns));
}

} // namespace vgd
