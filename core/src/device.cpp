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

    VkPhysicalDeviceDescriptorHeapFeaturesEXT heap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    heap.descriptorHeap = VK_TRUE;
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
                                               phys.desc.descriptor_heap ? &heap : nullptr};
    m5.maintenance5 = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &m5};
    v13.synchronization2 = VK_TRUE;
    v13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
    v12.timelineSemaphore = VK_TRUE;
    v12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};

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
#define VGD_VK_DEVICE(n) vk.n = reinterpret_cast<PFN_##n>(gdpa(impl->device, #n));
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
    log(LogLevel::info, "device on %s: direct family %u, compute %u, copy %u", phys.desc.name.c_str(),
        fam.direct, fam.compute, fam.copy);

    out->reset(new Device(std::move(impl)));
    return S_OK;
}

Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Device::~Device() = default;
const AdapterDesc& Device::adapter() const { return impl_->physical->desc; }

// --- Resources ---------------------------------------------------------------------------------------------

Resource::Impl::~Impl()
{
    auto& vk = device->vk;
    if (mapped)
        vk.vkUnmapMemory(device->device, memory);
    if (buffer)
        vk.vkDestroyBuffer(device->device, buffer, nullptr);
    if (memory)
        vk.vkFreeMemory(device->device, memory, nullptr);
}

Resource::Resource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Resource::~Resource() = default;
uint64_t Resource::size() const { return impl_->size; }
D3D12_GPU_VIRTUAL_ADDRESS Resource::gpu_virtual_address() const { return impl_->address; }
void* Resource::cpu_pointer() const { return impl_->mapped; }

HRESULT Device::create_committed_buffer(D3D12_HEAP_TYPE heap_type, uint64_t size, D3D12_RESOURCE_FLAGS flags,
                                        std::unique_ptr<Resource>* out)
{
    if (!out || size == 0)
        return E_INVALIDARG;
    if (heap_type != D3D12_HEAP_TYPE_DEFAULT && heap_type != D3D12_HEAP_TYPE_UPLOAD &&
        heap_type != D3D12_HEAP_TYPE_READBACK)
        return E_NOTIMPL;
    Impl& d = *impl_;
    auto res = std::make_unique<Resource::Impl>();
    res->device = &d;
    res->size = size;

    // D3D12 buffers carry no usage at creation, so every usage the buffer could be bound with is declared.
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        bci.usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    // D3D12 has no queue-family ownership, so resources are shared concurrently across our families.
    if (d.families.size() > 1) {
        bci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bci.queueFamilyIndexCount = static_cast<uint32_t>(d.families.size());
        bci.pQueueFamilyIndices = d.families.data();
    }
    VkResult r = d.vk.vkCreateBuffer(d.device, &bci, nullptr, &res->buffer);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);

    VkMemoryRequirements req;
    d.vk.vkGetBufferMemoryRequirements(d.device, res->buffer, &req);
    const uint32_t type = d.find_memory_type(req.memoryTypeBits, heap_type);
    if (type == UINT32_MAX)
        return E_OUTOFMEMORY;
    VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags_info};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    r = d.vk.vkAllocateMemory(d.device, &mai, nullptr, &res->memory);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    r = d.vk.vkBindBufferMemory(d.device, res->buffer, res->memory, 0);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);

    VkBufferDeviceAddressInfo bda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bda.buffer = res->buffer;
    res->address = d.vk.vkGetBufferDeviceAddress(d.device, &bda);

    if (heap_type != D3D12_HEAP_TYPE_DEFAULT) {
        r = d.vk.vkMapMemory(d.device, res->memory, 0, VK_WHOLE_SIZE, 0, &res->mapped);
        if (r != VK_SUCCESS)
            return hresult_from_vk(r);
    }

    out->reset(new Resource(std::move(res)));
    return S_OK;
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
