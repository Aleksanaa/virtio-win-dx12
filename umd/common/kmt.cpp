// SPDX-License-Identifier: MIT
#include "kmt.h"
#include "log.h"
#include "viogpu_abi.h"

#include <winternl.h>   // OBJECT_ATTRIBUTES, for D3DKMTShareObjects

#include <new>
#include <vector>

namespace vgd::umd {

std::unique_ptr<KmtDevice> KmtDevice::open()
{
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi)
        return nullptr;
    std::unique_ptr<KmtDevice> k(new (std::nothrow) KmtDevice);
    if (!k)
        return nullptr;
    auto get = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(gdi, name)));
        return fn != nullptr;
    };
    if (!get(k->enum_adapters_, "D3DKMTEnumAdapters2") || !get(k->query_adapter_info_, "D3DKMTQueryAdapterInfo") ||
        !get(k->close_adapter_, "D3DKMTCloseAdapter") || !get(k->create_device_, "D3DKMTCreateDevice") ||
        !get(k->destroy_device_, "D3DKMTDestroyDevice") ||
        !get(k->query_resource_info_, "D3DKMTQueryResourceInfoFromNtHandle") ||
        !get(k->open_resource_, "D3DKMTOpenResourceFromNtHandle") ||
        !get(k->destroy_allocation_, "D3DKMTDestroyAllocation") || !get(k->share_objects_, "D3DKMTShareObjects")) {
        VGD_WARN("kmt: gdi32 lacks the D3DKMT entry points we need");
        return nullptr;
    }

    D3DKMT_ENUMADAPTERS2 e{};
    if (k->enum_adapters_(&e) < 0 || !e.NumAdapters)
        return nullptr;
    std::vector<D3DKMT_ADAPTERINFO> adapters(e.NumAdapters);
    e.pAdapters = adapters.data();
    if (k->enum_adapters_(&e) < 0)
        return nullptr;
    for (ULONG i = 0; i < e.NumAdapters; i++) {
        viogpu::AdapterInfo info{};
        D3DKMT_QUERYADAPTERINFO q{};
        q.hAdapter = adapters[i].hAdapter;
        q.Type = KMTQAITYPE_UMDRIVERPRIVATE;
        q.pPrivateDriverData = &info;
        q.PrivateDriverDataSize = sizeof(info);
        const bool ours = !k->adapter_ && k->query_adapter_info_(&q) >= 0 && info.IamVioGPU == viogpu::kIam;
        if (ours) {
            k->adapter_ = adapters[i].hAdapter;
        } else {
            D3DKMT_CLOSEADAPTER ca{adapters[i].hAdapter};
            if (k->close_adapter_(&ca) < 0)
                VGD_WARN("kmt: D3DKMTCloseAdapter(0x%x) failed", adapters[i].hAdapter);
        }
    }
    if (!k->adapter_) {
        VGD_WARN("kmt: no viogpu adapter among %lu", e.NumAdapters);
        return nullptr;
    }
    D3DKMT_CREATEDEVICE cd{};
    cd.hAdapter = k->adapter_;
    NTSTATUS s = k->create_device_(&cd);
    if (s < 0) {
        VGD_WARN("kmt: D3DKMTCreateDevice failed: 0x%08lx", s);
        return nullptr;
    }
    k->device_ = cd.hDevice;
    VGD_TRACE("kmt: kernel device 0x%x on adapter 0x%x", k->device_, k->adapter_);
    return k;
}

KmtDevice::~KmtDevice()
{
    if (device_) {
        D3DKMT_DESTROYDEVICE dd{device_};
        if (destroy_device_(&dd) < 0)
            VGD_WARN("kmt: D3DKMTDestroyDevice(0x%x) failed", device_);
    }
    if (adapter_) {
        D3DKMT_CLOSEADAPTER ca{adapter_};
        if (close_adapter_(&ca) < 0)
            VGD_WARN("kmt: D3DKMTCloseAdapter(0x%x) failed", adapter_);
    }
}

HRESULT KmtDevice::open_shared(HANDLE nt, D3DKMT_HANDLE* resource, D3DKMT_HANDLE* allocation)
{
    D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE q{};
    q.hDevice = device_;
    q.hNtHandle = nt;
    NTSTATUS s = query_resource_info_(&q);
    if (s < 0 || q.NumAllocations != 1) {
        VGD_WARN("kmt: QueryResourceInfoFromNtHandle failed: 0x%08lx, %u allocations", s, q.NumAllocations);
        return E_FAIL;
    }
    std::vector<char> runtime(q.PrivateRuntimeDataSize), total(q.TotalPrivateDriverDataSize),
        res_private(q.ResourcePrivateDriverDataSize);
    D3DDDI_OPENALLOCATIONINFO2 info{};
    D3DKMT_OPENRESOURCEFROMNTHANDLE o{};
    o.hDevice = device_;
    o.hNtHandle = nt;
    o.NumAllocations = 1;
    o.pOpenAllocationInfo2 = &info;
    o.PrivateRuntimeDataSize = q.PrivateRuntimeDataSize;
    o.pPrivateRuntimeData = runtime.data();
    o.ResourcePrivateDriverDataSize = q.ResourcePrivateDriverDataSize;
    o.pResourcePrivateDriverData = res_private.data();
    o.TotalPrivateDriverDataBufferSize = q.TotalPrivateDriverDataSize;
    o.pTotalPrivateDriverDataBuffer = total.data();
    s = open_resource_(&o);
    if (s < 0 || !info.hAllocation) {
        VGD_WARN("kmt: OpenResourceFromNtHandle failed: 0x%08lx", s);
        return E_FAIL;
    }
    *resource = o.hResource;
    *allocation = info.hAllocation;
    return S_OK;
}

void KmtDevice::close(D3DKMT_HANDLE resource)
{
    D3DKMT_DESTROYALLOCATION d{};
    d.hDevice = device_;
    d.hResource = resource;
    NTSTATUS s = destroy_allocation_(&d);
    if (s < 0)
        VGD_WARN("kmt: closing opened resource 0x%x failed: 0x%08lx", resource, s);
}

HRESULT KmtDevice::share(D3DKMT_HANDLE resource, HANDLE* nt)
{
    OBJECT_ATTRIBUTES attrs{};
    attrs.Length = sizeof(attrs);
    *nt = nullptr;
    NTSTATUS s = share_objects_(1, &resource, &attrs, SHARED_ALLOCATION_ALL_ACCESS, nt);
    if (s < 0 || !*nt) {
        VGD_WARN("kmt: D3DKMTShareObjects(0x%x) failed: 0x%08lx", resource, s);
        return E_FAIL;
    }
    return S_OK;
}

} // namespace vgd::umd
