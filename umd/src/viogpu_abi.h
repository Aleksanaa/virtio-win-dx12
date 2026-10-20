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

#pragma pack(push, 1)
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
        UINT Reserved : 25;
    } Flags;
    ULONGLONG SupportedCapsetIDs;   // bit n set: capset id n is supported
};
#pragma pack(pop)

static_assert(sizeof(AdapterInfo) == 20, "must match viogpum.h VIOGPU_ADAPTERINFO");

} // namespace vgd::viogpu
