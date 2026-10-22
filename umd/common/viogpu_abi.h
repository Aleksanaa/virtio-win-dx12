// SPDX-License-Identifier: BSD-3-Clause
// The parts of yttrium's kernel/user interface (viogpu/shared/viogpum.h, Copyright (C) 2019-2020 Red Hat, Inc.,
// BSD-3-Clause) this UMD depends on. We pin the yttrium release; any change here is an interface bump.
#pragma once

#include <windows.h>

namespace vgd::viogpu {

constexpr ULONGLONG kIam = 0x56696f475055;   // "VioGPU": set by the KMD in its QueryAdapterInfo private data

constexpr unsigned kCapsetVirgl = 1;
constexpr unsigned kCapsetVirgl2 = 2;
constexpr unsigned kCapsetVenus = 4;

// D3DKMTEscape requests (VIOGPU_ESCAPE): type, data length, request. The KMD wants the whole union's size at least,
// which the padding covers.
constexpr USHORT kEscapeResourceInfo = 0x100;   // VIOGPU_RES_INFO: the host resource id an allocation names
#pragma pack(push, 1)
struct Escape {
    USHORT Type;
    USHORT DataLength;
    union {
        struct {
            UINT ResHandle;   // D3DKMT_HANDLE of an allocation of the escaping device
            ULONG Id;
        } ResourceInfo;
        BYTE Pad[124];
    };
};
#pragma pack(pop)

struct AdapterInfo {
    ULONGLONG IamVioGPU;
    struct {
        UINT Supports3d : 1;
        UINT has_capset_query_fix : 1;
        UINT has_resource_blob : 1;
        UINT has_host_visible : 1;
        UINT has_resource_assign_uuid : 1;
        UINT has_context_init : 1;
        UINT requires_explicit_residency : 1;
        UINT supports_allocation_alias : 1;   // our KMD patch (kmd/): AllocationAlias
        UINT Reserved : 24;
    } Flags;
    ULONGLONG SupportedCapsetIDs;   // bit n set: capset id n is supported
};

// Natural alignment: viogpum.h's pack(1) blocks do not cover this struct (8 + 4 + 4 padding + 8).
static_assert(sizeof(AdapterInfo) == 24, "must match viogpum.h VIOGPU_ADAPTERINFO");

#pragma pack(push, 1)
// viogpum.h VIOGPU_RESOURCE_OPTIONS: a virgl resource description.
struct ResourceOptions {
    ULONG target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags;
};

// viogpum.h VIOGPU_CREATE_ALLOCATION_EXCHANGE: allocation private data, read back by every opener.
struct AllocationExchange {
    ResourceOptions options;
    ULONGLONG Size;
    ULONGLONG BlobId;
    ULONG BlobMem;
    ULONG BlobFlags;
    ULONG Stride;
    ULONG ScanoutOffset;
};

// viogpum.h VIOGPU_CREATE_ALLOCATION_ALIAS (our KMD patch): an allocation sharing the host resource of
// SourceAllocation, an allocation handle of this process.
constexpr ULONG kAllocationAliasMagic = 0x53414756;   // "VGAS"
struct AllocationAlias {
    AllocationExchange exchange;
    ULONG Magic;
    UINT SourceAllocation;   // D3DKMT_HANDLE
};
#pragma pack(pop)
static_assert(sizeof(AllocationExchange) == 72, "must match viogpum.h VIOGPU_CREATE_ALLOCATION_EXCHANGE");
static_assert(sizeof(AllocationAlias) == 80, "must match viogpum.h VIOGPU_CREATE_ALLOCATION_ALIAS");

// virtio-gpu blob memory and flags.
constexpr ULONG kBlobMemHost3d = 2;
constexpr ULONG kBlobFlagShareable = 2;

// What yttrium's D3D10 UMD decodes when it opens an allocation (and writes in those it shares): a gallium target,
// gallium bind flags (we set the first four, which have the same values in virgl; scanout does not, and is left out)
// and a virgl format.
constexpr ULONG kPipeTexture2d = 2;
constexpr ULONG kBindRenderTarget = 1u << 1, kBindSamplerView = 1u << 3, kBindDisplayTarget = 1u << 7,
                kBindShaderImage = 1u << 15, kBindShared = 1u << 20;
constexpr ULONG kVirglFormatB8G8R8A8Unorm = 1, kVirglFormatB8G8R8X8Unorm = 2, kVirglFormatR10G10B10A2Unorm = 8,
                kVirglFormatA8Unorm = 10, kVirglFormatR8Unorm = 64, kVirglFormatR8G8B8A8Unorm = 67,
                kVirglFormatB8G8R8A8Srgb = 100, kVirglFormatR8G8B8A8Srgb = 104;
// The importer (yttrium_resource_to_pipe_format) translates only the virgl formats above and takes any other value
// as a Mesa pipe_format, whose numbering is Mesa's own: this is PIPE_FORMAT_R16G16B16A16_FLOAT in the Mesa of
// third_party/virtio-win-mesa (its generated u_format_gen.h), the one yttrium's D3D10 UMD is built from.
constexpr ULONG kPipeFormatR16G16B16A16Float = 56;

} // namespace vgd::viogpu
