// SPDX-License-Identifier: MIT
// Adapter caps (PFND3D12DDI_GETCAPS) for DDI 0092. Answers are deliberately conservative: everything we have not
// implemented reports "not supported", and the feature level only claims what the core will really provide.
#include "caps.h"
#include "log.h"

#include <cstring>

namespace vgd::umd {

namespace {

// FL 12_0 requires tiled resources tier 2 (a non-goal for now), so the honest level is 11_1.
constexpr D3D12DDI_3DPIPELINELEVEL kFeatureLevel = D3D12DDI_3DPIPELINELEVEL_11_1;

// Shader models dxil-spirv translates and we are ready to claim; raised as the shader path matures.
constexpr D3D12DDI_SHADER_MODEL kShaderModels[] = {
    D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011,
    D3D12DDI_SHADER_MODEL_6_0_RELEASE_0011,
};

template <class T>
T* payload(const D3D12DDIARG_GETCAPS& r)
{
    if (!r.pData || r.DataSize != sizeof(T)) {
        VGD_WARN("GetCaps type=%d: DataSize %u, expected %u", static_cast<int>(r.Type), r.DataSize,
                 static_cast<unsigned>(sizeof(T)));
        return nullptr;
    }
    return static_cast<T*>(r.pData);
}

bool node_zero(const D3D12DDIARG_GETCAPS& r)
{
    return r.pInfo && *static_cast<const UINT*>(r.pInfo) == 0;
}

} // namespace

HRESULT get_caps(const D3D12DDIARG_GETCAPS& r)
{
    switch (r.Type) {
    case D3D12DDICAPS_TYPE_3DPIPELINESUPPORT: {
        auto* d = payload<D3D12DDI_3DPIPELINELEVEL>(r);
        if (!d) return E_INVALIDARG;
        *d = kFeatureLevel;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0081_3DPIPELINESUPPORT1: {
        // The highest level we support that does not exceed what the runtime understands.
        auto* d = payload<D3D12DDI_3DPIPELINESUPPORT1_DATA_0081>(r);
        if (!d) return E_INVALIDARG;
        d->MaximumDriverSupportedFeatureLevel =
            d->HighestRuntimeSupportedFeatureLevel < kFeatureLevel ? d->HighestRuntimeSupportedFeatureLevel : kFeatureLevel;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0011_SHADER_MODELS: {
        // Called twice: first for the count, then with an array of that size.
        auto* d = payload<D3D12DDI_D3D12_SHADER_MODELS_DATA_0011>(r);
        if (!d || !d->pNumShaderModelsSupported) return E_INVALIDARG;
        constexpr UINT n = ARRAYSIZE(kShaderModels);
        if (d->pShaderModelsSupported) {
            if (*d->pNumShaderModelsSupported < n) return E_INVALIDARG;
            for (UINT i = 0; i < n; i++)
                d->pShaderModelsSupported[i] = kShaderModels[i];
        }
        *d->pNumShaderModelsSupported = n;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_D3D12_OPTIONS: {
        auto* d = payload<D3D12DDI_D3D12_OPTIONS_DATA_0089>(r);
        if (!d) return E_INVALIDARG;
        *d = {};
        // VK_EXT_descriptor_heap gives us heap-indexed descriptors; tier 2 is the safe claim until full tier 3
        // semantics (unbounded, partially bound tables) are tested.
        d->ResourceBindingTier = D3D12DDI_RESOURCE_BINDING_TIER_2;
        // Tier 2 (mixed buffers/textures in one heap) needs placed resources first.
        d->ResourceHeapTier = D3D12DDI_RESOURCE_HEAP_TIER_1;
        // Everything else (tiled resources, conservative raster, VRS, mesh, raytracing, render passes, enhanced
        // barriers, ...) stays zero: not supported.
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_SHADER: {
        auto* d = payload<D3D12DDI_SHADER_CAPS_0084>(r);
        if (!d) return E_INVALIDARG;
        *d = {};
        d->MinPrecision = D3D12DDI_SHADER_MIN_PRECISION_NONE;
        d->WaveLaneCountMin = 32;   // reported only; WaveOps stays FALSE until wave intrinsics are wired
        d->WaveLaneCountMax = 64;
        d->TotalLaneCount = 1024;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_ARCHITECTURE_INFO: {
        auto* d = payload<D3D12DDI_ARCHITECTURE_INFO_DATA>(r);
        if (!d) return E_INVALIDARG;
        d->TileBasedDeferredRenderer = FALSE;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE: {
        // Through Venus the guest sees separate device memory and host-visible blobs: a discrete-style adapter.
        auto* d = payload<D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041>(r);
        if (!d || !node_zero(r)) return E_INVALIDARG;
        *d = {};
        d->HeapSerializationTier = D3D12DDI_HEAP_SERIALIZATION_TIER_0041_0;
        d->ResourceSerializationTier = D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_0;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_GPUVA_CAPS: {
        auto* d = payload<D3D12DDI_GPUVA_CAPS_0004>(r);
        if (!d) return E_INVALIDARG;
        d->MaxGPUVirtualAddressBitsPerResource = 40;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0022_TEXTURE_LAYOUT: {
        // No device-dependent layouts or swizzles, no standard swizzle, no row-major textures (yet).
        auto* d = payload<D3D12DDI_TEXTURE_LAYOUT_CAPS_0026>(r);
        if (!d) return E_INVALIDARG;
        *d = {};
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS: {
        auto* d = payload<D3D12DDI_ROW_MAJOR_LAYOUT_CAPS>(r);
        if (!d) return E_INVALIDARG;
        *d = {};
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0022_SWIZZLE_PATTERN:
        return E_INVALIDARG;   // we report zero device-dependent swizzle patterns, so no index is valid
    case D3D12DDICAPS_TYPE_0022_CPU_PAGE_TABLE_FALSE_POSITIVES: {
        auto* d = payload<D3D12DDI_COMMAND_QUEUE_FLAGS>(r);
        if (!d || !node_zero(r)) return E_INVALIDARG;
        *d = static_cast<D3D12DDI_COMMAND_QUEUE_FLAGS>(D3D12DDI_COMMAND_QUEUE_FLAG_3D | D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE |
                                                        D3D12DDI_COMMAND_QUEUE_FLAG_COPY);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0030_PROTECTED_RESOURCE_SESSION_SUPPORT: {
        auto* d = payload<D3D12DDI_PROTECTED_RESOURCE_SESSION_SUPPORT_DATA_0030>(r);
        if (!d || d->NodeIndex != 0) return E_INVALIDARG;
        d->Support = D3D12DDI_PROTECTED_RESOURCE_SESSION_SUPPORT_FLAG_0030_NONE;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_EXECUTECOMMANDLISTS_PARALLELISM:
    case D3D12DDICAPS_TYPE_0073_SUPPORT_BATCHED_MARKERS: {
        auto* d = payload<BOOL>(r);
        if (!d) return E_INVALIDARG;
        *d = FALSE;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0023_UMD_BASED_COMMAND_QUEUE_PRIORITY: {
        auto* d = payload<D3D12DDICAPS_UMD_BASED_COMMAND_QUEUE_PRIORITY_DATA_0023>(r);
        if (!d) return E_INVALIDARG;
        d->SupportedQueueFlagsForGlobalRealtimeQueues = D3D12DDI_COMMAND_QUEUE_FLAG_NONE;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0050_HARDWARE_SCHEDULING_CAPS: {
        auto* d = payload<D3D12DDICAPS_HARDWARE_SCHEDULING_CAPS_0050>(r);
        if (!d) return E_INVALIDARG;
        d->ComputeQueuesPer3DQueue = 0;   // no scheduling groups
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0090: {
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0090>(r);
        if (!d) return E_INVALIDARG;
        *d = {};
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0091: {
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0091>(r);
        if (!d) return E_INVALIDARG;
        *d = {};
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_SHADER_MODEL_6_8_OPTIONS_0110: {
        auto* d = payload<D3D12DDI_SHADER_MODEL_6_8_OPTIONS_0110>(r);
        if (!d) return E_INVALIDARG;
        *d = {};
        return S_OK;
    }
    default:
        // Bring-up: an unknown query gets an all-zero answer ("not supported") so one run shows every query the
        // runtime makes. Each one logged here needs a real decision.
        VGD_WARN("GetCaps type=%d size=%u: UNHANDLED, answered with zeros", static_cast<int>(r.Type), r.DataSize);
        if (!r.pData) return E_INVALIDARG;
        std::memset(r.pData, 0, r.DataSize);
        return S_OK;
    }
}

} // namespace vgd::umd
