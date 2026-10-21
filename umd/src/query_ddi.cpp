// SPDX-License-Identifier: MIT
// Query heaps and queries, on core query heaps (core/src/queries.cpp).
#include "device.h"
#include "log.h"
#include "objects.h"

#include <new>

namespace vgd::umd {

namespace {

SIZE_T APIENTRY calc_private_query_heap_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_QUERY_HEAP_0001*)
{
    return sizeof(QueryHeapObject);
}

HRESULT APIENTRY create_query_heap(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_QUERY_HEAP_0001* args,
                                   D3D12DDI_HQUERYHEAP hq)
{
    auto* obj = new (hq.pDrvPrivate) QueryHeapObject;
    const D3D12_QUERY_HEAP_DESC desc{static_cast<D3D12_QUERY_HEAP_TYPE>(args->Type), args->Count, args->NodeMask};
    HRESULT hr = Device::from(h)->core->create_query_heap(desc, &obj->heap);
    if (FAILED(hr)) {
        VGD_WARN("CreateQueryHeap: type %d, %u queries failed: 0x%08lx", static_cast<int>(args->Type), args->Count, hr);
        obj->~QueryHeapObject();
        return hr == E_NOTIMPL ? E_INVALIDARG : E_OUTOFMEMORY;
    }
    VGD_VERBOSE("CreateQueryHeap: type %d, %u queries", static_cast<int>(args->Type), args->Count);
    return S_OK;
}

void APIENTRY destroy_query_heap(D3D12DDI_HDEVICE, D3D12DDI_HQUERYHEAP hq)
{
    object<QueryHeapObject>(hq)->~QueryHeapObject();
}

vgd::CommandList& list_of(D3D12DDI_HCOMMANDLIST hl)
{
    return *object<CommandListObject>(hl)->list;
}

vgd::QueryHeap& heap_of(D3D12DDI_HQUERYHEAP hq)
{
    return *object<QueryHeapObject>(hq)->heap;
}

void APIENTRY begin_query(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HQUERYHEAP hq, D3D12DDI_QUERY_TYPE type, UINT index)
{
    list_of(hl).begin_query(heap_of(hq), static_cast<D3D12_QUERY_TYPE>(type), index);
}

void APIENTRY end_query(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HQUERYHEAP hq, D3D12DDI_QUERY_TYPE type, UINT index)
{
    list_of(hl).end_query(heap_of(hq), static_cast<D3D12_QUERY_TYPE>(type), index);
}

void APIENTRY resolve_query_data(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HQUERYHEAP hq, D3D12DDI_QUERY_TYPE type,
                                 UINT start, UINT count, D3D12DDI_HRESOURCE dst, UINT64 offset)
{
    list_of(hl).resolve_query_data(heap_of(hq), static_cast<D3D12_QUERY_TYPE>(type), start, count,
                                   *object<ResourceObject>(dst)->resource, offset);
}

} // namespace

void fill_query_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnCalcPrivateQueryHeapSize = calc_private_query_heap_size;
    t.pfnCreateQueryHeap = create_query_heap;
    t.pfnDestroyQueryHeap = destroy_query_heap;
}

void fill_query_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t)
{
    t.pfnBeginQuery = begin_query;
    t.pfnEndQuery = end_query;
    t.pfnResolveQueryData = resolve_query_data;
}

} // namespace vgd::umd
