// SPDX-License-Identifier: MIT
// Descriptor heaps, views and samplers.
//
// The bindless model: a shader-visible CBV/SRV/UAV heap is one Vulkan descriptor set whose single binding is an
// array of mutable descriptors (any of the six buffer and image types), one per heap slot; a sampler heap is an array
// of samplers. Descriptor tables become an offset into that array, passed in the root data, and dxil-spirv indexes
// the array from there. CPU-only heaps are just our records; copying into a shader-visible heap writes the Vulkan
// descriptors from them.
#include "internal.h"

#include <algorithm>
#include <cstring>

namespace vgd {

namespace {

constexpr VkDescriptorType kMutableTypes[] = {
    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
};

// D3D12 heap limits: resource binding tier 2 allows a million CBV/SRV/UAV descriptors in a shader-visible heap, and
// every tier 2048 samplers.
constexpr uint32_t kMaxResourceDescriptors = 1'000'000;
constexpr uint32_t kMaxSamplerDescriptors = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;

VkComponentSwizzle swizzle_of(uint32_t d3d)
{
    switch (d3d) {
    case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0: return VK_COMPONENT_SWIZZLE_R;
    case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1: return VK_COMPONENT_SWIZZLE_G;
    case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2: return VK_COMPONENT_SWIZZLE_B;
    case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3: return VK_COMPONENT_SWIZZLE_A;
    case D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
    }
}

// D3D's 4-component mapping as our packed VkComponentMapping. A stencil view reads stencil in .g in D3D but in .r
// in Vulkan, so memory component 1 becomes R there.
uint32_t pack_swizzle(UINT mapping, bool stencil)
{
    uint32_t packed = 0;
    for (int c = 0; c < 4; c++) {
        uint32_t src = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(c, mapping);
        VkComponentSwizzle s = swizzle_of(src);
        if (stencil && src <= 3)
            s = src == 1 ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_ZERO;
        packed |= uint32_t(s) << (8 * c);
    }
    return packed;
}

bool is_stencil_view_format(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT || f == DXGI_FORMAT_X24_TYPELESS_G8_UINT;
}

// The buffer a view covers: [first, first + count) elements of `stride` bytes.
bool buffer_range(Resource::Impl& r, uint64_t first, uint32_t count, uint32_t stride, VkDeviceSize* offset,
                  VkDeviceSize* range)
{
    *offset = first * stride;
    *range = uint64_t(count) * stride;
    return r.buffer && stride && *offset + *range <= r.size;
}

VkSamplerAddressMode address_mode(D3D12_TEXTURE_ADDRESS_MODE m)
{
    switch (m) {
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case D3D12_TEXTURE_ADDRESS_MODE_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

} // namespace

VkDescriptorType vk_descriptor_type(Descriptor::Kind kind)
{
    switch (kind) {
    case Descriptor::Kind::sampled_image: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case Descriptor::Kind::storage_image: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case Descriptor::Kind::uniform_texel: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    case Descriptor::Kind::storage_texel: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    case Descriptor::Kind::uniform_buffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case Descriptor::Kind::storage_buffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case Descriptor::Kind::sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    default: return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

// --- Bindless set layouts ----------------------------------------------------------------------------------

HRESULT Device::Impl::create_bindless_layouts()
{
    VkDescriptorSetLayoutCreateInfo empty{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkResult er = vk.vkCreateDescriptorSetLayout(device, &empty, nullptr, &empty_set_layout);
    if (er != VK_SUCCESS)
        return hresult_from_vk(er);
    if (!physical->mutable_descriptors)
        return S_OK;
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    ivk->vkGetPhysicalDeviceProperties2(physical->handle, &props);
    max_resource_descriptors = std::min({kMaxResourceDescriptors, p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                         p12.maxPerStageDescriptorUpdateAfterBindStorageImages,
                                         p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
                                         p12.maxPerStageDescriptorUpdateAfterBindUniformBuffers,
                                         p12.maxUpdateAfterBindDescriptorsInAllPools});
    max_sampler_descriptors = std::min(kMaxSamplerDescriptors, p12.maxPerStageDescriptorUpdateAfterBindSamplers);

    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 1;
    bf.pBindingFlags = &flags;
    const VkMutableDescriptorTypeListEXT list{static_cast<uint32_t>(std::size(kMutableTypes)), kMutableTypes};
    VkMutableDescriptorTypeCreateInfoEXT mut{VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, &bf};
    mut.mutableDescriptorTypeListCount = 1;
    mut.pMutableDescriptorTypeLists = &list;

    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, max_resource_descriptors, VK_SHADER_STAGE_ALL};
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &mut};
    ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    ci.bindingCount = 1;
    ci.pBindings = &b;
    VkResult r = vk.vkCreateDescriptorSetLayout(device, &ci, nullptr, &resource_set_layout);
    if (r != VK_SUCCESS) {
        log(LogLevel::warn, "bindless resource set layout (%u descriptors) failed: %d", max_resource_descriptors, r);
        return hresult_from_vk(r);
    }
    b = {0, VK_DESCRIPTOR_TYPE_SAMPLER, max_sampler_descriptors, VK_SHADER_STAGE_ALL};
    ci.pNext = &bf;
    r = vk.vkCreateDescriptorSetLayout(device, &ci, nullptr, &sampler_set_layout);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);
    log(LogLevel::info, "bindless descriptors: %u resources, %u samplers per heap", max_resource_descriptors,
        max_sampler_descriptors);
    return S_OK;
}

// --- Samplers ----------------------------------------------------------------------------------------------

VkSampler Device::Impl::get_sampler(const D3D12_SAMPLER_DESC& d)
{
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const auto linear = [](int f) { return f == D3D12_FILTER_TYPE_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; };
    ci.magFilter = linear(D3D12_DECODE_MAG_FILTER(d.Filter));
    ci.minFilter = linear(D3D12_DECODE_MIN_FILTER(d.Filter));
    ci.mipmapMode = D3D12_DECODE_MIP_FILTER(d.Filter) == D3D12_FILTER_TYPE_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                                   : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = address_mode(d.AddressU);
    ci.addressModeV = address_mode(d.AddressV);
    ci.addressModeW = address_mode(d.AddressW);
    ci.mipLodBias = d.MipLODBias;
    if (D3D12_DECODE_IS_ANISOTROPIC_FILTER(d.Filter) && physical->features.samplerAnisotropy) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::clamp<float>(static_cast<float>(d.MaxAnisotropy), 1.0f,
                                             physical->limits.maxSamplerAnisotropy);
        ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
    const auto reduction = D3D12_DECODE_FILTER_REDUCTION(d.Filter);
    if (reduction == D3D12_FILTER_REDUCTION_TYPE_COMPARISON) {
        ci.compareEnable = VK_TRUE;
        ci.compareOp = d.ComparisonFunc ? static_cast<VkCompareOp>(d.ComparisonFunc - 1) : VK_COMPARE_OP_NEVER;
    }
    ci.minLod = d.MinLOD;
    ci.maxLod = d.MaxLOD;
    // Standard border colors only (custom ones need VK_EXT_custom_border_color).
    const float* c = d.BorderColor;
    if (c[3] == 0.0f)
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f)
        ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else
        ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VkSamplerReductionModeCreateInfo rm{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    if (reduction == D3D12_FILTER_REDUCTION_TYPE_MINIMUM || reduction == D3D12_FILTER_REDUCTION_TYPE_MAXIMUM) {
        rm.reductionMode = reduction == D3D12_FILTER_REDUCTION_TYPE_MINIMUM ? VK_SAMPLER_REDUCTION_MODE_MIN
                                                                            : VK_SAMPLER_REDUCTION_MODE_MAX;
        if (physical->v12.samplerFilterMinmax)
            ci.pNext = &rm;
    }

    // Cached by every field after pNext, plus the reduction mode (the only pNext we chain).
    SamplerKey key{};
    std::memcpy(&key.fields, &ci.flags, sizeof(key.fields));
    key.reduction = ci.pNext ? rm.reductionMode : VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
    std::lock_guard g(samplers_lock);
    for (const auto& [k, s] : samplers)
        if (!std::memcmp(&k, &key, sizeof(key)))
            return s;
    VkSampler s = VK_NULL_HANDLE;
    VkResult r = vk.vkCreateSampler(device, &ci, nullptr, &s);
    if (r != VK_SUCCESS) {
        log(LogLevel::warn, "vkCreateSampler failed: %d", r);
        return VK_NULL_HANDLE;
    }
    samplers.push_back({key, s});
    return s;
}

HRESULT Device::create_sampler(const D3D12_SAMPLER_DESC& desc, Descriptor* out)
{
    *out = {};
    out->kind = Descriptor::Kind::sampler;
    out->sampler = impl_->get_sampler(desc);
    return out->sampler ? S_OK : E_OUTOFMEMORY;
}

// --- Views -------------------------------------------------------------------------------------------------

VkBufferView Resource::Impl::buffer_view(const BufferViewKey& key)
{
    std::lock_guard g(views_lock);
    for (auto& [k, v] : buffer_views)
        if (k == key)
            return v;
    VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    ci.buffer = buffer;
    ci.format = key.format;
    ci.offset = key.offset;
    ci.range = key.range;
    VkBufferView v = VK_NULL_HANDLE;
    VkResult r = device->vk.vkCreateBufferView(device->device, &ci, nullptr, &v);
    if (r != VK_SUCCESS) {
        log(LogLevel::warn, "vkCreateBufferView (format %d) failed: %d", key.format, r);
        return VK_NULL_HANDLE;
    }
    buffer_views.push_back({key, v});
    return v;
}

HRESULT Device::create_shader_resource_view(Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                            Descriptor* out)
{
    *out = {};
    if (!resource && !desc)
        return E_INVALIDARG;
    const D3D12_SRV_DIMENSION dim = desc ? desc->ViewDimension
                                         : resource->is_texture() ? D3D12_SRV_DIMENSION_UNKNOWN
                                                                  : D3D12_SRV_DIMENSION_BUFFER;
    if (dim == D3D12_SRV_DIMENSION_BUFFER) {
        const bool raw = desc && (desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW);
        const bool structured = desc && desc->Buffer.StructureByteStride;
        out->kind = raw || structured ? Descriptor::Kind::storage_buffer : Descriptor::Kind::uniform_texel;
        if (!resource)
            return S_OK;   // null descriptor
        Resource::Impl& r = resource->impl();
        const uint32_t stride = raw ? 4 : structured ? desc->Buffer.StructureByteStride
                                                    : format_block(impl_->resolve_format(desc->Format)).bytes;
        VkDeviceSize offset, range;
        if (!desc || !buffer_range(r, desc->Buffer.FirstElement, desc->Buffer.NumElements, stride, &offset, &range))
            return E_INVALIDARG;
        if (out->kind == Descriptor::Kind::storage_buffer) {
            out->buffer = {r.buffer, offset, range};
        } else {
            out->texel = r.buffer_view({impl_->resolve_format(desc->Format), offset, range});
            if (!out->texel)
                return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    out->kind = Descriptor::Kind::sampled_image;
    if (!resource)
        return S_OK;
    Resource::Impl& r = resource->impl();
    if (!r.image)
        return E_INVALIDARG;
    const auto& rd = r.desc;
    ImageViewKey key{VK_IMAGE_VIEW_TYPE_2D, r.format, r.aspects & ~VK_IMAGE_ASPECT_STENCIL_BIT, 0, rd.MipLevels, 0,
                     r.array_layers(), 0, VK_IMAGE_USAGE_SAMPLED_BIT};
    const DXGI_FORMAT view_format = desc && desc->Format != DXGI_FORMAT_UNKNOWN ? desc->Format : rd.Format;
    if (r.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        // Depth/stencil images are viewed in their own format; the D3D view format only picks the plane.
        const bool stencil = is_stencil_view_format(view_format) ||
                             (desc && dim == D3D12_SRV_DIMENSION_TEXTURE2D && desc->Texture2D.PlaneSlice == 1);
        key.aspects = stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        key.swizzle = pack_swizzle(desc ? desc->Shader4ComponentMapping : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                   stencil);
    } else {
        key.format = impl_->resolve_format(view_format);
        if (desc && desc->Shader4ComponentMapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)
            key.swizzle = pack_swizzle(desc->Shader4ComponentMapping, false);
    }
    auto mips = [&](UINT most_detailed, UINT levels) {
        key.mip = most_detailed;
        key.mips = levels == UINT_MAX ? rd.MipLevels - most_detailed : levels;
    };
    auto layers = [&](UINT first, UINT size) {
        key.layer = first;
        key.layers = size == UINT_MAX ? r.array_layers() - first : size;
    };
    switch (dim) {
    case D3D12_SRV_DIMENSION_UNKNOWN:   // no desc: the whole resource
        key.type = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? VK_IMAGE_VIEW_TYPE_3D
                   : rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                       ? (key.layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                       : (key.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        key.type = VK_IMAGE_VIEW_TYPE_1D;
        mips(desc->Texture1D.MostDetailedMip, desc->Texture1D.MipLevels);
        key.layers = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        key.type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        mips(desc->Texture1DArray.MostDetailedMip, desc->Texture1DArray.MipLevels);
        layers(desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize);
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        key.type = VK_IMAGE_VIEW_TYPE_2D;
        mips(desc->Texture2D.MostDetailedMip, desc->Texture2D.MipLevels);
        key.layers = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        key.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        mips(desc->Texture2DArray.MostDetailedMip, desc->Texture2DArray.MipLevels);
        layers(desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize);
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:
        key.type = VK_IMAGE_VIEW_TYPE_2D;
        key.mips = key.layers = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
        key.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        key.mips = 1;
        layers(desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize);
        break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        key.type = VK_IMAGE_VIEW_TYPE_3D;
        mips(desc->Texture3D.MostDetailedMip, desc->Texture3D.MipLevels);
        key.layers = 1;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        key.type = VK_IMAGE_VIEW_TYPE_CUBE;
        mips(desc->TextureCube.MostDetailedMip, desc->TextureCube.MipLevels);
        key.layers = 6;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        key.type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        mips(desc->TextureCubeArray.MostDetailedMip, desc->TextureCubeArray.MipLevels);
        key.layer = desc->TextureCubeArray.First2DArrayFace;
        key.layers = desc->TextureCubeArray.NumCubes == UINT_MAX ? (r.array_layers() - key.layer) / 6 * 6
                                                                 : 6 * desc->TextureCubeArray.NumCubes;
        break;
    default:
        log(LogLevel::warn, "shader resource view dimension %d not supported", dim);
        return E_INVALIDARG;
    }
    if (key.format == VK_FORMAT_UNDEFINED || key.mip + key.mips > rd.MipLevels || key.mips == 0 ||
        key.layer + key.layers > r.array_layers() || key.layers == 0)
        return E_INVALIDARG;
    out->image = r.view(key);
    return out->image ? S_OK : E_OUTOFMEMORY;
}

HRESULT Device::create_unordered_access_view(Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                             Descriptor* out)
{
    *out = {};
    if (!resource && !desc)
        return E_INVALIDARG;
    const D3D12_UAV_DIMENSION dim = desc ? desc->ViewDimension
                                         : resource->is_texture() ? D3D12_UAV_DIMENSION_UNKNOWN
                                                                  : D3D12_UAV_DIMENSION_BUFFER;
    if (dim == D3D12_UAV_DIMENSION_BUFFER) {
        const bool raw = desc && (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW);
        const bool structured = desc && desc->Buffer.StructureByteStride;
        out->kind = raw || structured ? Descriptor::Kind::storage_buffer : Descriptor::Kind::storage_texel;
        if (!resource)
            return S_OK;
        Resource::Impl& r = resource->impl();
        const uint32_t stride = raw ? 4 : structured ? desc->Buffer.StructureByteStride
                                                    : format_block(impl_->resolve_format(desc->Format)).bytes;
        VkDeviceSize offset, range;
        if (!desc || !buffer_range(r, desc->Buffer.FirstElement, desc->Buffer.NumElements, stride, &offset, &range))
            return E_INVALIDARG;
        if (out->kind == Descriptor::Kind::storage_buffer) {
            out->buffer = {r.buffer, offset, range};
        } else {
            out->texel = r.buffer_view({impl_->resolve_format(desc->Format), offset, range});
            if (!out->texel)
                return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    out->kind = Descriptor::Kind::storage_image;
    if (!resource)
        return S_OK;
    Resource::Impl& r = resource->impl();
    if (!r.image)
        return E_INVALIDARG;
    const auto& rd = r.desc;
    const DXGI_FORMAT view_format = desc && desc->Format != DXGI_FORMAT_UNKNOWN ? desc->Format : rd.Format;
    ImageViewKey key{VK_IMAGE_VIEW_TYPE_2D, impl_->resolve_format(view_format), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                     1, 0, VK_IMAGE_USAGE_STORAGE_BIT};
    auto layers = [&](UINT first, UINT size) {
        key.layer = first;
        key.layers = size == UINT_MAX ? r.array_layers() - first : size;
    };
    switch (dim) {
    case D3D12_UAV_DIMENSION_UNKNOWN:
        key.layers = r.array_layers();
        key.type = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? VK_IMAGE_VIEW_TYPE_3D
                   : key.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE1D:
        key.type = VK_IMAGE_VIEW_TYPE_1D;
        key.mip = desc->Texture1D.MipSlice;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
        key.type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        key.mip = desc->Texture1DArray.MipSlice;
        layers(desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize);
        break;
    case D3D12_UAV_DIMENSION_TEXTURE2D:
        key.mip = desc->Texture2D.MipSlice;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
        key.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        key.mip = desc->Texture2DArray.MipSlice;
        layers(desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize);
        break;
    case D3D12_UAV_DIMENSION_TEXTURE3D:
        // A 3D UAV sees its depth slices through a 3D view; only whole-depth views are exact.
        key.type = VK_IMAGE_VIEW_TYPE_3D;
        key.mip = desc->Texture3D.MipSlice;
        if (desc->Texture3D.FirstWSlice != 0)
            log(LogLevel::warn, "3D UAV starting at depth slice %u: the whole depth is visible",
                desc->Texture3D.FirstWSlice);
        break;
    default:
        log(LogLevel::warn, "unordered access view dimension %d not supported", dim);
        return E_INVALIDARG;
    }
    if (key.format == VK_FORMAT_UNDEFINED || key.mip >= rd.MipLevels || key.layer + key.layers > r.array_layers() ||
        key.layers == 0)
        return E_INVALIDARG;
    out->image = r.view(key);
    return out->image ? S_OK : E_OUTOFMEMORY;
}

HRESULT Device::create_constant_buffer_view(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, Descriptor* out)
{
    *out = {};
    out->kind = Descriptor::Kind::uniform_buffer;
    if (!desc.BufferLocation)
        return S_OK;   // null CBV
    VkDeviceSize offset, left;
    const VkBuffer b = impl_->find_buffer(desc.BufferLocation, &offset, &left);
    if (!b || desc.SizeInBytes > left) {
        log(LogLevel::warn, "CBV at 0x%llx (%u bytes) is not inside one buffer",
            static_cast<unsigned long long>(desc.BufferLocation), desc.SizeInBytes);
        return E_INVALIDARG;
    }
    out->buffer = {b, offset, desc.SizeInBytes};
    return S_OK;
}

// --- Heaps -------------------------------------------------------------------------------------------------

DescriptorHeap::DescriptorHeap(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DescriptorHeap::~DescriptorHeap() = default;
D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeap::type() const { return impl_->type; }
uint32_t DescriptorHeap::count() const { return impl_->count; }
bool DescriptorHeap::shader_visible() const { return impl_->set != VK_NULL_HANDLE; }
Descriptor* DescriptorHeap::cpu_start() { return impl_->records.get(); }
uint64_t DescriptorHeap::gpu_start() const
{
    return impl_->set ? reinterpret_cast<uint64_t>(impl_->records.get()) : 0;
}

DescriptorHeap::Impl::~Impl()
{
    {
        std::unique_lock g(device->heaps_lock);
        device->heaps_by_address.erase(reinterpret_cast<uintptr_t>(records.get()));
    }
    if (pool)
        device->vk.vkDestroyDescriptorPool(device->device, pool, nullptr);
}

DescriptorHeap::Impl* Device::Impl::find_heap(const Descriptor* d)
{
    const auto a = reinterpret_cast<uintptr_t>(d);
    std::shared_lock g(heaps_lock);
    auto it = heaps_by_address.upper_bound(a);
    if (it == heaps_by_address.begin())
        return nullptr;
    --it;
    DescriptorHeap::Impl* h = it->second;
    return a < it->first + uintptr_t(h->count) * sizeof(Descriptor) ? h : nullptr;
}

HRESULT Device::create_descriptor_heap(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shader_visible,
                                       std::unique_ptr<DescriptorHeap>* out)
{
    Impl& d = *impl_;
    auto h = std::make_unique<DescriptorHeap::Impl>();
    h->device = &d;
    h->type = type;
    h->count = std::max(count, 1u);
    // The records are Descriptor objects kDescriptorStride apart: the array stride must be exactly that.
    static_assert(sizeof(Descriptor) <= kDescriptorStride);
    h->records.reset(new (std::nothrow) Descriptor[h->count]);
    if (!h->records)
        return E_OUTOFMEMORY;

    const bool sampler = type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    if (shader_visible && (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || sampler)) {
        if (!d.resource_set_layout) {
            log(LogLevel::warn, "shader-visible descriptor heap without mutable descriptors; tables will not work");
        } else if (h->count > (sampler ? d.max_sampler_descriptors : d.max_resource_descriptors)) {
            log(LogLevel::warn, "shader-visible heap of %u descriptors exceeds %u", h->count,
                sampler ? d.max_sampler_descriptors : d.max_resource_descriptors);
            return E_OUTOFMEMORY;
        } else {
            const VkMutableDescriptorTypeListEXT list{static_cast<uint32_t>(std::size(kMutableTypes)), kMutableTypes};
            VkMutableDescriptorTypeCreateInfoEXT mut{VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT};
            mut.mutableDescriptorTypeListCount = 1;
            mut.pMutableDescriptorTypeLists = &list;
            const VkDescriptorPoolSize size{sampler ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_MUTABLE_EXT,
                                            h->count};
            VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, sampler ? nullptr : &mut};
            pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
            pci.maxSets = 1;
            pci.poolSizeCount = 1;
            pci.pPoolSizes = &size;
            VkResult r = d.vk.vkCreateDescriptorPool(d.device, &pci, nullptr, &h->pool);
            if (r != VK_SUCCESS)
                return hresult_from_vk(r);
            VkDescriptorSetVariableDescriptorCountAllocateInfo var{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
            var.descriptorSetCount = 1;
            var.pDescriptorCounts = &h->count;
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &var};
            ai.descriptorPool = h->pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = sampler ? &d.sampler_set_layout : &d.resource_set_layout;
            r = d.vk.vkAllocateDescriptorSets(d.device, &ai, &h->set);
            if (r != VK_SUCCESS)
                return hresult_from_vk(r);
        }
    }
    {
        std::unique_lock g(d.heaps_lock);
        d.heaps_by_address[reinterpret_cast<uintptr_t>(h->records.get())] = h.get();
    }
    out->reset(new DescriptorHeap(std::move(h)));
    return S_OK;
}

void DescriptorHeap::Impl::flush(uint32_t first, uint32_t n)
{
    if (!set)
        return;
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkDescriptorBufferInfo> buffers;
    writes.reserve(n);
    images.reserve(n);
    buffers.reserve(n);
    for (uint32_t i = first; i < first + n && i < count; i++) {
        const Descriptor& d = records[i];
        const VkDescriptorType type = vk_descriptor_type(d.kind);
        if (type == VK_DESCRIPTOR_TYPE_MAX_ENUM)
            continue;   // never written, or a kind that does not belong in this heap
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstArrayElement = i;
        w.descriptorCount = 1;
        w.descriptorType = type;
        switch (d.kind) {
        case Descriptor::Kind::sampled_image:
        case Descriptor::Kind::storage_image:
            if (!d.image && !device->physical->null_descriptor)
                continue;
            images.push_back({VK_NULL_HANDLE, d.image, VK_IMAGE_LAYOUT_GENERAL});
            w.pImageInfo = &images.back();
            break;
        case Descriptor::Kind::sampler:
            images.push_back({d.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
            w.pImageInfo = &images.back();
            break;
        case Descriptor::Kind::uniform_texel:
        case Descriptor::Kind::storage_texel:
            if (!d.texel && !device->physical->null_descriptor)
                continue;
            w.pTexelBufferView = &d.texel;
            break;
        default:
            if (!d.buffer.buffer && !device->physical->null_descriptor)
                continue;
            buffers.push_back({d.buffer.buffer, d.buffer.offset, d.buffer.buffer ? d.buffer.range : VK_WHOLE_SIZE});
            w.pBufferInfo = &buffers.back();
            break;
        }
        writes.push_back(w);
    }
    if (writes.empty())
        return;
    std::lock_guard g(set_lock);
    device->vk.vkUpdateDescriptorSets(device->device, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                                      nullptr);
}

void Device::write_descriptor(Descriptor* dst, const Descriptor& value)
{
    *dst = value;
    if (DescriptorHeap::Impl* h = impl_->find_heap(dst))
        h->flush(static_cast<uint32_t>(dst - h->records.get()), 1);
}

void Device::copy_descriptors(Descriptor* dst, const Descriptor* src, uint32_t count)
{
    std::memmove(static_cast<void*>(dst), src, sizeof(Descriptor) * count);
    if (DescriptorHeap::Impl* h = impl_->find_heap(dst))
        h->flush(static_cast<uint32_t>(dst - h->records.get()), count);
}

} // namespace vgd
