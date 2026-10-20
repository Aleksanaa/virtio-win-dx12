// SPDX-License-Identifier: MIT
// Command pools, recorders, lists and queues (the DDI model since 0040):
//  - a pool is untyped; it owns one core allocator per queue type, created when first needed;
//  - a recorder (the API's allocator, as the runtime sees it) has a queue type and a target pool;
//  - ResetCommandList names a recorder; the list then records into the target pool's allocator for its type.
#include "device.h"
#include "log.h"
#include "objects.h"

#include <array>
#include <new>

namespace vgd::umd {

namespace {

constexpr size_t kQueueTypes = 3;   // D3D12_COMMAND_LIST_TYPE_DIRECT, COMPUTE, COPY map to slots 0..2

bool queue_type(D3D12DDI_COMMAND_QUEUE_FLAGS flags, D3D12_COMMAND_LIST_TYPE* type)
{
    if (flags & D3D12DDI_COMMAND_QUEUE_FLAG_3D)
        *type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    else if (flags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE)
        *type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    else if (flags & D3D12DDI_COMMAND_QUEUE_FLAG_COPY)
        *type = D3D12_COMMAND_LIST_TYPE_COPY;
    else
        return false;
    return true;
}

size_t slot_of(D3D12_COMMAND_LIST_TYPE type)
{
    return type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1 : type == D3D12_COMMAND_LIST_TYPE_COPY ? 2 : 0;
}

struct CommandPoolObject {
    Device* device;
    std::array<std::unique_ptr<vgd::CommandAllocator>, kQueueTypes> allocators;
};

struct CommandRecorderObject {
    D3D12_COMMAND_LIST_TYPE type;
    CommandPoolObject* pool = nullptr;
};

struct CommandListObject {
    Device* device;
    D3D12DDI_HRTCOMMANDLIST rt;
    D3D12_COMMAND_LIST_TYPE type;
    std::unique_ptr<vgd::CommandList> list;

    void error(HRESULT hr) { device->um_callbacks->pfnSetCommandListErrorCb(rt, hr); }
};

struct CommandQueueObject {
    std::unique_ptr<vgd::CommandQueue> queue;
};

// --- Pools ---------------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_command_pool_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_POOL_0040*)
{
    return sizeof(CommandPoolObject);
}

HRESULT APIENTRY create_command_pool(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_COMMAND_POOL_0040*,
                                     D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    new (hp.pDrvPrivate) CommandPoolObject{Device::from(h), {}};
    return S_OK;
}

void APIENTRY destroy_command_pool(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    object<CommandPoolObject>(hp)->~CommandPoolObject();
}

void APIENTRY reset_command_pool(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    for (auto& a : object<CommandPoolObject>(hp)->allocators)
        if (a && FAILED(a->reset())) {
            Device* d = Device::from(h);
            d->um_callbacks->pfnSetErrorCb(d->rt_device, E_OUTOFMEMORY);
        }
}

// --- Recorders -----------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_command_recorder_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040*)
{
    return sizeof(CommandRecorderObject);
}

HRESULT APIENTRY create_command_recorder(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040* args,
                                         D3D12DDI_HCOMMANDRECORDER_0040 hr_)
{
    D3D12_COMMAND_LIST_TYPE type;
    if (!queue_type(args->QueueFlags, &type)) {
        VGD_WARN("CreateCommandRecorder: unsupported queue flags 0x%x", static_cast<unsigned>(args->QueueFlags));
        return E_OUTOFMEMORY;
    }
    new (hr_.pDrvPrivate) CommandRecorderObject{type};
    return S_OK;
}

void APIENTRY destroy_command_recorder(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDRECORDER_0040 hr_)
{
    object<CommandRecorderObject>(hr_)->~CommandRecorderObject();
}

void APIENTRY set_command_pool_as_target(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDRECORDER_0040 hr_,
                                         D3D12DDI_HCOMMANDPOOL_0040 hp)
{
    object<CommandRecorderObject>(hr_)->pool = hp.pDrvPrivate ? object<CommandPoolObject>(hp) : nullptr;
}

// --- Lists ---------------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_command_list_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_LIST_0040*)
{
    return sizeof(CommandListObject);
}

HRESULT APIENTRY create_command_list(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_COMMAND_LIST_0040* args,
                                     D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HRTCOMMANDLIST rt)
{
    D3D12_COMMAND_LIST_TYPE type;
    if (args->Type == D3D12DDI_COMMAND_LIST_TYPE_BUNDLE || !queue_type(args->QueueFlags, &type)) {
        VGD_WARN("CreateCommandList: unsupported type %d / queue flags 0x%x (bundles come later)",
                 static_cast<int>(args->Type), static_cast<unsigned>(args->QueueFlags));
        return E_OUTOFMEMORY;
    }
    Device* d = Device::from(h);
    auto* obj = new (hl.pDrvPrivate) CommandListObject{d, rt, type, nullptr};
    if (FAILED(d->core->create_command_list(type, &obj->list))) {
        obj->~CommandListObject();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

void APIENTRY destroy_command_list(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDLIST hl)
{
    object<CommandListObject>(hl)->~CommandListObject();
}

void APIENTRY close_command_list(D3D12DDI_HCOMMANDLIST hl)
{
    auto* l = object<CommandListObject>(hl);
    if (FAILED(l->list->close()))
        l->error(E_FAIL);
}

void APIENTRY reset_command_list(D3D12DDI_HCOMMANDLIST hl, const D3D12DDIARG_RESETCOMMANDLIST_0040* args)
{
    auto* l = object<CommandListObject>(hl);
    auto* rec = object<CommandRecorderObject>(args->hDrvCommandRecorder);
    if (!rec->pool) {
        VGD_WARN("ResetCommandList: recorder has no target pool");
        l->error(E_INVALIDARG);
        return;
    }
    auto& alloc = rec->pool->allocators[slot_of(l->type)];
    if (!alloc && FAILED(l->device->core->create_command_allocator(l->type, &alloc))) {
        l->error(E_OUTOFMEMORY);
        return;
    }
    if (FAILED(l->list->reset(*alloc)))
        l->error(E_OUTOFMEMORY);
}

// --- Queues --------------------------------------------------------------------------------------------------

SIZE_T APIENTRY calc_private_command_queue_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATECOMMANDQUEUE_0050*)
{
    return sizeof(CommandQueueObject);
}

HRESULT APIENTRY create_command_queue(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATECOMMANDQUEUE_0050* args,
                                      D3D12DDI_HCOMMANDQUEUE hq, D3D12DDI_HRTCOMMANDQUEUE)
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    if (!queue_type(args->QueueFlags, &desc.Type)) {
        VGD_WARN("CreateCommandQueue: unsupported queue flags 0x%x", static_cast<unsigned>(args->QueueFlags));
        return E_OUTOFMEMORY;
    }
    auto* obj = new (hq.pDrvPrivate) CommandQueueObject;
    if (FAILED(Device::from(h)->core->create_command_queue(desc, &obj->queue))) {
        obj->~CommandQueueObject();
        return E_OUTOFMEMORY;
    }
    VGD_TRACE("CreateCommandQueue: flags 0x%x", static_cast<unsigned>(args->QueueFlags));
    return S_OK;
}

void APIENTRY destroy_command_queue(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDQUEUE hq)
{
    object<CommandQueueObject>(hq)->~CommandQueueObject();
}

void APIENTRY execute_command_lists(D3D12DDI_HCOMMANDQUEUE hq, UINT count, const D3D12DDI_HCOMMANDLIST* lists)
{
    std::vector<vgd::CommandList*> core_lists(count);
    for (UINT i = 0; i < count; i++)
        core_lists[i] = object<CommandListObject>(lists[i])->list.get();
    HRESULT hr = object<CommandQueueObject>(hq)->queue->execute_command_lists(core_lists);
    if (FAILED(hr))
        VGD_WARN("ExecuteCommandLists(%u) failed: 0x%08lx", count, hr);
}

} // namespace

void fill_command_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnCalcPrivateCommandPoolSize = calc_private_command_pool_size;
    t.pfnCreateCommandPool = create_command_pool;
    t.pfnDestroyCommandPool = destroy_command_pool;
    t.pfnResetCommandPool = reset_command_pool;
    t.pfnCalcPrivateCommandRecorderSize = calc_private_command_recorder_size;
    t.pfnCreateCommandRecorder = create_command_recorder;
    t.pfnDestroyCommandRecorder = destroy_command_recorder;
    t.pfnCommandRecorderSetCommandPoolAsTarget = set_command_pool_as_target;
    t.pfnCalcPrivateCommandListSize = calc_private_command_list_size;
    t.pfnCreateCommandList = create_command_list;
    t.pfnDestroyCommandList = destroy_command_list;
    t.pfnCalcPrivateCommandQueueSize = calc_private_command_queue_size;
    t.pfnCreateCommandQueue = create_command_queue;
    t.pfnDestroyCommandQueue = destroy_command_queue;
}

void fill_command_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t)
{
    t.pfnCloseCommandList = close_command_list;
    t.pfnResetCommandList = reset_command_list;
}

void fill_command_queue_ddi(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001& t)
{
    t.pfnExecuteCommandLists = execute_command_lists;
}

} // namespace vgd::umd
