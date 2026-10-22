// SPDX-License-Identifier: MIT
#pragma once

#include "engine.h"
#include "kmt.h"
#include "viogpu_abi.h"
#include "wdk.h"

#include <memory>

namespace vgd::umd {

// Runtime handles of the command-list tables FillDDITable filled, by table number. Every new command list must be bound
// to one with SetCommandListDDITableCb, or the runtime calls through an empty table (first seen as a null call in Close).
struct CommandListTables {
    static constexpr UINT kMax = 4;
    D3D12DDI_HRTTABLE rt[kMax]{};
    UINT count = 0;
};

// Lives in the runtime-allocated storage behind D3D12DDI_HDEVICE (sized by CalcPrivateDeviceSize).
struct Device {
    D3D12DDI_HRTDEVICE rt_device{};
    const CommandListTables* command_list_tables = nullptr;   // owned by the adapter
    const D3DDDI_DEVICECALLBACKS* kt_callbacks = nullptr;                 // kernel thunks (allocations, fences, ...)
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062* um_callbacks = nullptr; // runtime callbacks in user mode
    std::shared_ptr<Engine> engine;
    ID3D12Device* d3d = nullptr;   // engine->device
    // Newer interfaces of the same device, for what only they create; null if vkd3d-proton lacks them.
    Com<ID3D12Device2> d3d2;
    Com<ID3D12Device4> d3d4;
    Com<ID3D12Device8> d3d8;
    // The KMD understands viogpu::AllocationAlias (our patch): swapchain buffers can be presented.
    bool allocation_alias = false;
    // Our own kernel device, for opening shared handles (present.cpp); created on first use.
    std::unique_ptr<KmtDevice> kmt;

    static Device* from(D3D12DDI_HDEVICE h) { return static_cast<Device*>(h.pDrvPrivate); }
    // Void DDIs report failure through the runtime's error callback.
    void set_error(HRESULT hr) const { um_callbacks->pfnSetErrorCb(rt_device, hr); }
};

SIZE_T device_private_size();
HRESULT create_device(const D3D12DDIARG_CREATEDEVICE_0003& args, const CommandListTables& tables,
                      const viogpu::AdapterInfo& info, std::shared_ptr<Engine> engine);
void destroy_device(D3D12DDI_HDEVICE device);
// `index` is the table number (the runtime asks for several command-list tables); command-list table handles are
// recorded in `tables`.
HRESULT fill_ddi_table(D3D12DDI_TABLE_TYPE type, void* table, SIZE_T size, UINT index, D3D12DDI_HRTTABLE rt_table,
                       CommandListTables& tables);

} // namespace vgd::umd
