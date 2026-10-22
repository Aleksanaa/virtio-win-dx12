// SPDX-License-Identifier: MIT
// Adapter caps (PFND3D12DDI_GETCAPS) for DDI 0092, from what vkd3d-proton reports through CheckFeatureSupport.
// A feature is passed on only when the DDI functions it brings are implemented; the others stay "not supported"
// even where vkd3d-proton has them: ray tracing (state objects, acceleration structures), mesh shaders (their
// signatures need names from the DXIL metadata, shader_container.h), sampler feedback, enhanced barriers and render
// passes (the runtime emulates those). The feature level is capped to match.
#include "caps.h"
#include "log.h"

#include <algorithm>
#include <cstring>

namespace vgd::umd {

namespace {

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

template <class T>
T feature(ID3D12Device* d, D3D12_FEATURE f)
{
    T data{};
    if (FAILED(d->CheckFeatureSupport(f, &data, sizeof(data))))
        data = {};
    return data;
}

// Shader models DDI 0092 can name: through 6.7.
constexpr D3D12DDI_SHADER_MODEL kShaderModels[] = {
    D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011, D3D12DDI_SHADER_MODEL_6_0_RELEASE_0011,
    D3D12DDI_SHADER_MODEL_6_1_RELEASE_0033, D3D12DDI_SHADER_MODEL_6_2_RELEASE_0042,
    D3D12DDI_SHADER_MODEL_6_3_RELEASE_0054, D3D12DDI_SHADER_MODEL_6_4_RELEASE_0062,
    D3D12DDI_SHADER_MODEL_6_5_RELEASE_0071, D3D12DDI_SHADER_MODEL_6_6_RELEASE_0082,
    D3D12DDI_SHADER_MODEL_6_7_RELEASE_0093,
};

// How many of kShaderModels the device supports.
UINT shader_model_count(ID3D12Device* d)
{
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_7};
    if (FAILED(d->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        sm.HighestShaderModel = D3D_SHADER_MODEL_5_1;
    const UINT minor = sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0 ? (sm.HighestShaderModel & 0xf) : 0;
    return sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0 ? std::min<UINT>(2 + minor, ARRAYSIZE(kShaderModels)) : 1;
}

// The DDI's queue flags of the API's command list types (D3D12_COMMAND_LIST_SUPPORT_FLAGS): different bits. Bundles
// run on the queue of the list that executes them, so they add nothing.
D3D12DDI_COMMAND_QUEUE_FLAGS queue_flags(D3D12_COMMAND_LIST_SUPPORT_FLAGS f)
{
    UINT q = D3D12DDI_COMMAND_QUEUE_FLAG_NONE;
    if (f & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT)
        q |= D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    if (f & D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE)
        q |= D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    if (f & D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY)
        q |= D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    return static_cast<D3D12DDI_COMMAND_QUEUE_FLAGS>(q);
}

// The feature level of what we pass on (see the top of this file): at most 12_1, since 12_2 needs ray tracing,
// mesh shaders and sampler feedback.
D3D12DDI_3DPIPELINELEVEL feature_level(ID3D12Device* d)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                               D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{ARRAYSIZE(levels), levels};
    if (FAILED(d->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
        return D3D12DDI_3DPIPELINELEVEL_11_0;
    switch (fl.MaxSupportedFeatureLevel) {
    case D3D_FEATURE_LEVEL_11_1: return D3D12DDI_3DPIPELINELEVEL_11_1;
    case D3D_FEATURE_LEVEL_12_0: return D3D12DDI_3DPIPELINELEVEL_12_0;
    case D3D_FEATURE_LEVEL_12_1:
    case D3D_FEATURE_LEVEL_12_2: return D3D12DDI_3DPIPELINELEVEL_12_1;
    default: return D3D12DDI_3DPIPELINELEVEL_11_0;
    }
}

} // namespace

HRESULT get_caps(ID3D12Device* dev, const D3D12DDIARG_GETCAPS& r)
{
    switch (r.Type) {
    case D3D12DDICAPS_TYPE_3DPIPELINESUPPORT: {
        auto* d = payload<D3D12DDI_3DPIPELINELEVEL>(r);
        if (!d) return E_INVALIDARG;
        *d = feature_level(dev);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0081_3DPIPELINESUPPORT1: {
        // The highest level we support that does not exceed what the runtime understands.
        auto* d = payload<D3D12DDI_3DPIPELINESUPPORT1_DATA_0081>(r);
        if (!d) return E_INVALIDARG;
        d->MaximumDriverSupportedFeatureLevel = std::min(d->HighestRuntimeSupportedFeatureLevel, feature_level(dev));
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0011_SHADER_MODELS: {
        // Called twice: first for the count, then with an array of that size.
        auto* d = payload<D3D12DDI_D3D12_SHADER_MODELS_DATA_0011>(r);
        if (!d || !d->pNumShaderModelsSupported) return E_INVALIDARG;
        const UINT n = shader_model_count(dev);
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
        const auto o = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS>(dev, D3D12_FEATURE_D3D12_OPTIONS);
        const auto o2 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS2>(dev, D3D12_FEATURE_D3D12_OPTIONS2);
        const auto o3 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS3>(dev, D3D12_FEATURE_D3D12_OPTIONS3);
        const auto o4 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS4>(dev, D3D12_FEATURE_D3D12_OPTIONS4);
        const auto o5 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS5>(dev, D3D12_FEATURE_D3D12_OPTIONS5);
        const auto o6 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS6>(dev, D3D12_FEATURE_D3D12_OPTIONS6);
        const auto o10 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS10>(dev, D3D12_FEATURE_D3D12_OPTIONS10);
        *d = {};
        d->ResourceBindingTier = static_cast<D3D12DDI_RESOURCE_BINDING_TIER>(o.ResourceBindingTier);
        d->ConservativeRasterizationTier =
            static_cast<D3D12DDI_CONSERVATIVE_RASTERIZATION_TIER>(o.ConservativeRasterizationTier);
        // The DDI names tiers up to 3; tier 4 (64KB-aligned 3D tiles) is the API's alone.
        d->TiledResourcesTier = static_cast<D3D12DDI_TILED_RESOURCES_TIER>(
            std::min<int>(o.TiledResourcesTier, D3D12DDI_TILED_RESOURCES_TIER_3));
        d->CrossNodeSharingTier = D3D12DDI_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
        d->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation =
            o.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation;
        d->OutputMergerLogicOp = o.OutputMergerLogicOp;
        d->ResourceHeapTier = static_cast<D3D12DDI_RESOURCE_HEAP_TIER>(o.ResourceHeapTier);
        d->DepthBoundsTestSupported = o2.DepthBoundsTestSupported;
        d->ProgrammableSamplePositionsTier =
            static_cast<D3D12DDI_PROGRAMMABLE_SAMPLE_POSITIONS_TIER>(o2.ProgrammableSamplePositionsTier);
        d->CopyQueueTimestampQueriesSupported = o3.CopyQueueTimestampQueriesSupported;
        d->WriteBufferImmediateQueueFlags = queue_flags(o3.WriteBufferImmediateSupportFlags);
        d->ViewInstancingTier = static_cast<D3D12DDI_VIEW_INSTANCING_TIER>(o3.ViewInstancingTier);
        d->BarycentricsSupported = o3.BarycentricsSupported;
        d->ReservedBufferPlacementSupported = o4.MSAA64KBAlignedTextureSupported;
        d->SRVOnlyTiledResourceTier3 = o5.SRVOnlyTiledResourceTier3;
        d->VariableShadingRateTier = static_cast<D3D12DDI_VARIABLE_SHADING_RATE_TIER>(o6.VariableShadingRateTier);
        d->PerPrimitiveShadingRateSupportedWithViewportIndexing = o6.PerPrimitiveShadingRateSupportedWithViewportIndexing;
        d->AdditionalShadingRatesSupported = o6.AdditionalShadingRatesSupported;
        d->ShadingRateImageTileSize = o6.ShadingRateImageTileSize;
        d->VariableRateShadingSumCombinerSupported = o10.VariableRateShadingSumCombinerSupported;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_SHADER: {
        auto* d = payload<D3D12DDI_SHADER_CAPS_0084>(r);
        if (!d) return E_INVALIDARG;
        const auto o = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS>(dev, D3D12_FEATURE_D3D12_OPTIONS);
        const auto o1 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS1>(dev, D3D12_FEATURE_D3D12_OPTIONS1);
        const auto o4 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS4>(dev, D3D12_FEATURE_D3D12_OPTIONS4);
        const auto o9 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS9>(dev, D3D12_FEATURE_D3D12_OPTIONS9);
        const auto o11 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS11>(dev, D3D12_FEATURE_D3D12_OPTIONS11);
        *d = {};
        d->MinPrecision = static_cast<D3D12DDI_SHADER_MIN_PRECISION>(o.MinPrecisionSupport);
        d->DoubleOps = o.DoublePrecisionFloatShaderOps;
        d->ShaderSpecifiedStencilRef = o.PSSpecifiedStencilRefSupported;
        d->TypedUAVLoadAdditionalFormats = o.TypedUAVLoadAdditionalFormats;
        d->ROVs = o.ROVsSupported;
        d->WaveOps = o1.WaveOps;
        d->WaveLaneCountMin = o1.WaveLaneCountMin;
        d->WaveLaneCountMax = o1.WaveLaneCountMax;
        d->TotalLaneCount = o1.TotalLaneCount;
        d->Int64Ops = o1.Int64ShaderOps;
        d->Native16BitOps = o4.Native16BitShaderOpsSupported;
        d->AtomicInt64OnTypedResource = o9.AtomicInt64OnTypedResourceSupported;
        d->AtomicInt64OnGroupShared = o9.AtomicInt64OnGroupSharedSupported;
        d->AtomicInt64OnDescriptorHeapResource = o11.AtomicInt64OnDescriptorHeapResourceSupported;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0090: {
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0090>(r);
        if (!d) return E_INVALIDARG;
        const auto o12 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS12>(dev, D3D12_FEATURE_D3D12_OPTIONS12);
        *d = {};
        d->RelaxedFormatCastingSupported = o12.RelaxedFormatCastingSupported;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0091: {
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0091>(r);
        if (!d) return E_INVALIDARG;
        const auto o13 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS13>(dev, D3D12_FEATURE_D3D12_OPTIONS13);
        *d = {};
        d->UnrestrictedBufferTextureCopyPitchSupported = o13.UnrestrictedBufferTextureCopyPitchSupported;
        d->UnrestrictedVertexElementAlignmentSupported = o13.UnrestrictedVertexElementAlignmentSupported;
        d->InvertedViewportHeightFlipsYSupported = o13.InvertedViewportHeightFlipsYSupported;
        d->InvertedViewportDepthFlipsZSupported = o13.InvertedViewportDepthFlipsZSupported;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0093: {
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0093>(r);
        if (!d) return E_INVALIDARG;
        const auto o14 = feature<D3D12_FEATURE_DATA_D3D12_OPTIONS14>(dev, D3D12_FEATURE_D3D12_OPTIONS14);
        *d = {};
        d->AdvancedTextureOpsSupported = o14.AdvancedTextureOpsSupported;
        d->WriteableMSAATexturesSupported = o14.WriteableMSAATexturesSupported;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_ARCHITECTURE_INFO: {
        auto* d = payload<D3D12DDI_ARCHITECTURE_INFO_DATA>(r);
        if (!d) return E_INVALIDARG;
        d->TileBasedDeferredRenderer = FALSE;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE: {
        // Must agree with the KMD: yttrium describes only aperture (system memory) segments, no local video memory,
        // so Windows sees a UMA adapter. CacheCoherent stays FALSE: upload heaps are write-combined, not CPU cached.
        auto* d = payload<D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041>(r);
        if (!d || !node_zero(r)) return E_INVALIDARG;
        *d = {};
        d->UMA = TRUE;
        // Required by the runtime (CacheHeapCaps throws without it). True for us: the host-visible memory types
        // are HOST_COHERENT, so CPU and GPU see each other's writes without explicit cache maintenance.
        d->IOCoherent = TRUE;
        d->HeapSerializationTier = D3D12DDI_HEAP_SERIALIZATION_TIER_0041_0;
        d->ResourceSerializationTier = D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_0;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_GPUVA_CAPS: {
        auto* d = payload<D3D12DDI_GPUVA_CAPS_0004>(r);
        if (!d) return E_INVALIDARG;
        const auto va = feature<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT>(
            dev, D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT);
        d->MaxGPUVirtualAddressBitsPerResource = va.MaxGPUVirtualAddressBitsPerResource;
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
        // *pInfo = UINT[2] {D3D12DDI_TL_ROW_MAJOR, functional unit}. The runtime (CDevice::CacheHeapCaps) requires
        // SubCaps[0].MaxElementSize != 0 and power-of-two alignments. Row-major data moves through Vulkan
        // buffer-image copies, which need texel alignment only: one entry for every element size, alignment 1,
        // the same for every unit (also what Microsoft's CosUmd12 sample answers).
        auto* d = payload<D3D12DDI_ROW_MAJOR_LAYOUT_CAPS>(r);
        if (!d) return E_INVALIDARG;
        const UINT* key = static_cast<const UINT*>(r.pInfo);
        if (!key || key[0] != D3D12DDI_TL_ROW_MAJOR) return E_INVALIDARG;
        *d = {};
        d->SubCaps[0].MaxElementSize = 0xFFFF;
        d->SubCaps[0].BaseOffsetAlignment = 1;
        d->SubCaps[0].PitchAlignment = 1;
        d->SubCaps[0].DepthPitchAlignment = 1;
        d->Flags = D3D12DDI_ROW_MAJOR_LAYOUT_FLAG_NONE;
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
    case D3D12DDICAPS_TYPE_SHADER_MODEL_6_8_OPTIONS_0110: {
        auto* d = payload<D3D12DDI_SHADER_MODEL_6_8_OPTIONS_0110>(r);
        if (!d) return E_INVALIDARG;
        *d = {};
        return S_OK;
    }
    default:
        // An unknown query gets an all-zero answer ("not supported"). Each one logged here needs a real decision.
        VGD_WARN("GetCaps type=%d size=%u: UNHANDLED, answered with zeros", static_cast<int>(r.Type), r.DataSize);
        if (!r.pData) return E_INVALIDARG;
        std::memset(r.pData, 0, r.DataSize);
        return S_OK;
    }
}

} // namespace vgd::umd
