// SPDX-License-Identifier: MIT
#pragma once

#include "wdk.h"

#include <vgd/core.h>

#include <memory>

namespace vgd::umd {

// Lives in the runtime-allocated storage behind D3D12DDI_HDEVICE (sized by CalcPrivateDeviceSize).
// Runtime handles of the command-list tables FillDDITable filled, by table number. Every new command list must be bound
// to one with SetCommandListDDITableCb, or the runtime calls through an empty table (first seen as a null call in Close).
struct CommandListTables {
    static constexpr UINT kMax = 4;
    D3D12DDI_HRTTABLE rt[kMax]{};
    UINT count = 0;
};

struct Device {
    D3D12DDI_HRTDEVICE rt_device{};
    const CommandListTables* command_list_tables = nullptr;   // owned by the adapter
    const D3DDDI_DEVICECALLBACKS* kt_callbacks = nullptr;                 // kernel thunks (allocations, fences, ...)
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062* um_callbacks = nullptr; // runtime callbacks in user mode
    std::unique_ptr<vgd::Instance> instance;
    std::unique_ptr<vgd::Device> core;

    static Device* from(D3D12DDI_HDEVICE h) { return static_cast<Device*>(h.pDrvPrivate); }
};

SIZE_T device_private_size();
HRESULT create_device(const D3D12DDIARG_CREATEDEVICE_0003& args, const CommandListTables& tables);
void destroy_device(D3D12DDI_HDEVICE device);
// `index` is the table number (the runtime asks for several command-list tables); command-list table handles are
// recorded in `tables`.
HRESULT fill_ddi_table(D3D12DDI_TABLE_TYPE type, void* table, SIZE_T size, UINT index, D3D12DDI_HRTTABLE rt_table,
                       CommandListTables& tables);

} // namespace vgd::umd
