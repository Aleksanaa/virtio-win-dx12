// SPDX-License-Identifier: MIT
// Query heaps and queries, on vkd3d-proton's.
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
    HRESULT hr = Device::from(h)->d3d->CreateQueryHeap(&desc, __uuidof(ID3D12QueryHeap), obj->heap.put_void());
    if (FAILED(hr)) {
        VGD_WARN("CreateQueryHeap: type %d, %u queries failed: 0x%08lx", static_cast<int>(args->Type), args->Count, hr);
        obj->~QueryHeapObject();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

void APIENTRY destroy_query_heap(D3D12DDI_HDEVICE, D3D12DDI_HQUERYHEAP hq)
{
    object<QueryHeapObject>(hq)->~QueryHeapObject();
}

ID3D12GraphicsCommandList* list(D3D12DDI_HCOMMANDLIST hl)
{
    return object<CommandListObject>(hl)->list.get();
}

ID3D12QueryHeap* heap(D3D12DDI_HQUERYHEAP hq)
{
    return object<QueryHeapObject>(hq)->heap.get();
}

void APIENTRY begin_query(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HQUERYHEAP hq, D3D12DDI_QUERY_TYPE type, UINT index)
{
    list(hl)->BeginQuery(heap(hq), static_cast<D3D12_QUERY_TYPE>(type), index);
}

void APIENTRY end_query(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HQUERYHEAP hq, D3D12DDI_QUERY_TYPE type, UINT index)
{
    list(hl)->EndQuery(heap(hq), static_cast<D3D12_QUERY_TYPE>(type), index);
}

void APIENTRY resolve_query_data(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HQUERYHEAP hq, D3D12DDI_QUERY_TYPE type,
                                 UINT start, UINT count, D3D12DDI_HRESOURCE dst, UINT64 offset)
{
    list(hl)->ResolveQueryData(heap(hq), static_cast<D3D12_QUERY_TYPE>(type), start, count, resource_of(dst), offset);
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
