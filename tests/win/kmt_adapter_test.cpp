// SPDX-License-Identifier: MIT
#include "../../umd/common/kmt.h"
#include "../../umd/common/log.h"
#include "../../umd/common/viogpu_abi.h"
#include <winternl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace {
constexpr NTSTATUS invalid = static_cast<NTSTATUS>(0xc000000d);
std::set<UINT> adapters;
std::map<UINT, UINT> devices, resources;
UINT creates = 0, next_resource = 700;
bool fail_luid = false, fail_create = false, missing_luid = false;
UINT queried = 0, opened = 0;
const LUID owners[] = {{0x924d, 1}, {0x924d, 2}};
HANDLE handle(UINT n) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(n)); }
UINT owner(HANDLE h) { return h == handle(90) ? 10 : 20; }

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)

NTSTATUS APIENTRY enumerate(const D3DKMT_ENUMADAPTERS2* input)
{
    auto* a = const_cast<D3DKMT_ENUMADAPTERS2*>(input);
    if (a->pAdapters) {
        a->pAdapters[0] = {}; a->pAdapters[1] = {};
        a->pAdapters[0].hAdapter = 10; a->pAdapters[0].AdapterLuid = owners[0];
        a->pAdapters[1].hAdapter = 20; a->pAdapters[1].AdapterLuid = owners[1];
        adapters = {10, 20};
    }
    a->NumAdapters = 2;
    return 0;
}
NTSTATUS APIENTRY adapter_info(const D3DKMT_QUERYADAPTERINFO* a)
{
    if (!adapters.count(a->hAdapter)) return invalid;
    static_cast<vgd::viogpu::AdapterInfo*>(a->pPrivateDriverData)->IamVioGPU = vgd::viogpu::kIam;
    return 0;
}
NTSTATUS APIENTRY close_adapter(const D3DKMT_CLOSEADAPTER* a)
{
    return adapters.erase(a->hAdapter) == 1 ? 0 : invalid;
}
NTSTATUS APIENTRY create_device(D3DKMT_CREATEDEVICE* a)
{
    if (fail_create || !adapters.count(a->hAdapter)) return invalid;
    ++creates;
    a->hDevice = a->hAdapter + 100;
    devices[a->hDevice] = a->hAdapter;
    return 0;
}
NTSTATUS APIENTRY destroy_device(const D3DKMT_DESTROYDEVICE* a)
{
    return devices.erase(a->hDevice) == 1 ? 0 : invalid;
}
NTSTATUS APIENTRY shared_luid(D3DKMT_GETSHAREDRESOURCEADAPTERLUID* a)
{
    if (fail_luid) return invalid;
    a->AdapterLuid = owners[owner(a->hNtHandle) == 10 ? 0 : 1];
    return 0;
}
NTSTATUS APIENTRY query_resource(D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE* a)
{
    queried = a->hDevice;
    if (devices[a->hDevice] != owner(a->hNtHandle)) return invalid;
    a->NumAllocations = 1;
    a->PrivateRuntimeDataSize = 0; a->ResourcePrivateDriverDataSize = 4; a->TotalPrivateDriverDataSize = 72;
    return 0;
}
NTSTATUS APIENTRY open_resource(D3DKMT_OPENRESOURCEFROMNTHANDLE* a)
{
    opened = a->hDevice;
    if (devices[a->hDevice] != owner(a->hNtHandle)) return invalid;
    a->hResource = next_resource++;
    a->pOpenAllocationInfo2[0].hAllocation = a->hResource + 1000;
    resources[a->hResource] = a->hDevice;
    return 0;
}
NTSTATUS APIENTRY destroy_allocation(const D3DKMT_DESTROYALLOCATION* a)
{
    auto r = resources.find(a->hResource);
    if (r == resources.end() || r->second != a->hDevice) return invalid;
    resources.erase(r);
    return 0;
}
NTSTATUS APIENTRY share(UINT count, const D3DKMT_HANDLE* objects, POBJECT_ATTRIBUTES, DWORD access, HANDLE* out)
{
    if (count != 1) return invalid;
    if (*objects == 42 && access == GENERIC_ALL) {
        *out = handle(91);
        return 0;
    }
    auto r = resources.find(*objects);
    if (r == resources.end() || access != SHARED_ALLOCATION_ALL_ACCESS) return invalid;
    *out = handle(devices[r->second] == 10 ? 90 : 91);
    return 0;
}
NTSTATUS APIENTRY create_allocation(D3DKMT_CREATEALLOCATION* a)
{
    if (!devices.count(a->hDevice) || a->NumAllocations != 1 || !a->Flags.CreateResource ||
        !a->Flags.CreateShared || !a->Flags.NtSecuritySharing) return invalid;
    const auto* alias = static_cast<const vgd::viogpu::AllocationAlias*>(a->pAllocationInfo->pPrivateDriverData);
    if (alias->SourceAllocation != 55 || alias->Magic != vgd::viogpu::kAllocationAliasMagic) return invalid;
    a->hResource = next_resource++;
    resources[a->hResource] = a->hDevice;
    a->pAllocationInfo[0].hAllocation = a->hResource + 1000;
    return 0;
}
NTSTATUS APIENTRY escape(const D3DKMT_ESCAPE* a)
{
    auto* e = static_cast<vgd::viogpu::Escape*>(a->pPrivateDriverData);
    auto r = resources.find(e->ResourceInfo.ResHandle - 1000);
    if (r == resources.end() || r->second != a->hDevice || devices[a->hDevice] != a->hAdapter) return invalid;
    e->ResourceInfo.Id = 4321;
    return 0;
}
HMODULE test_load(LPCWSTR) { return reinterpret_cast<HMODULE>(1); }
FARPROC test_proc(HMODULE, LPCSTR name)
{
#define ENTRY(n, f, type) if (!std::strcmp(name, n)) { type checked = f; return reinterpret_cast<FARPROC>(reinterpret_cast<void*>(checked)); }
    ENTRY("D3DKMTEnumAdapters2", enumerate, PFND3DKMT_ENUMADAPTERS2)
    ENTRY("D3DKMTQueryAdapterInfo", adapter_info, PFND3DKMT_QUERYADAPTERINFO)
    ENTRY("D3DKMTCloseAdapter", close_adapter, PFND3DKMT_CLOSEADAPTER)
    ENTRY("D3DKMTCreateDevice", create_device, PFND3DKMT_CREATEDEVICE)
    ENTRY("D3DKMTDestroyDevice", destroy_device, PFND3DKMT_DESTROYDEVICE)
    if (missing_luid && !std::strcmp(name, "D3DKMTGetSharedResourceAdapterLuid")) return nullptr;
    ENTRY("D3DKMTGetSharedResourceAdapterLuid", shared_luid, PFND3DKMT_GETSHAREDRESOURCEADAPTERLUID)
    ENTRY("D3DKMTQueryResourceInfoFromNtHandle", query_resource, PFND3DKMT_QUERYRESOURCEINFOFROMNTHANDLE)
    ENTRY("D3DKMTOpenResourceFromNtHandle", open_resource, PFND3DKMT_OPENRESOURCEFROMNTHANDLE)
    ENTRY("D3DKMTDestroyAllocation", destroy_allocation, PFND3DKMT_DESTROYALLOCATION)
    ENTRY("D3DKMTShareObjects", share, PFND3DKMT_SHAREOBJECTS)
    ENTRY("D3DKMTCreateAllocation", create_allocation, PFND3DKMT_CREATEALLOCATION)
    ENTRY("D3DKMTEscape", escape, PFND3DKMT_ESCAPE)
#undef ENTRY
    return nullptr;
}
}

namespace vgd::umd { void logf(LogLevel, const char*, ...) {} }
// Compile the production implementation with its Windows loader boundary replaced.
#define LoadLibraryW test_load
#define GetProcAddress test_proc
#ifndef VGD_KMT_TEST_SOURCE
#define VGD_KMT_TEST_SOURCE "../../umd/common/kmt.cpp"
#endif
#include VGD_KMT_TEST_SOURCE
#undef GetProcAddress
#undef LoadLibraryW

int main()
{
    using vgd::umd::KmtDevice;
    auto k = KmtDevice::open();
    CHECK(k != nullptr);
    D3DKMT_HANDLE r = 0, a = 0;
    CHECK(k->open_shared(handle(91), &r, &a) == S_OK);
    CHECK(r == 700 && a == 1700);
    CHECK(queried == 120 && opened == 120);
    CHECK(creates == 1 && adapters == std::set<UINT>{20});
    CHECK(k->resource_id(a) == 4321);
    D3DKMT_HANDLE r2 = 0, a2 = 0;
    CHECK(k->open_shared(handle(92), &r2, &a2) == S_OK);
    CHECK(r2 == 701 && a2 == 1701 && creates == 1);
    CHECK(k->open_shared(handle(90), &r2, &a2) == E_FAIL);
    CHECK(r2 == 701 && a2 == 1701);
    CHECK(k->resource_id(a) == 4321 && k->resource_id(a2) == 4321);
    k->close(r2);
    CHECK(k->resource_id(a2) == 0 && k->resource_id(a) == 4321);
    k->close(r);
    CHECK(resources.size() == 0);
    k.reset();
    CHECK(devices.size() == 0 && adapters.size() == 0);
    std::puts("PASS owner selection, reuse, cross-owner rejection and resource lifetime");

    k = KmtDevice::open();
    HANDLE nt = nullptr;
    CHECK(k->share(42, GENERIC_ALL, &nt) == S_OK && nt == handle(91));
    CHECK(devices.size() == 0);
    fail_luid = true;
    r = 0; a = 0;
    CHECK(k->open_shared(nt, &r, &a) == E_FAIL && r == 0 && a == 0);
    CHECK(devices.size() == 0 && adapters.size() == 0);
    fail_luid = false;
    fail_create = true;
    CHECK(k->open_shared(nt, &r, &a) == E_FAIL && r == 0 && a == 0);
    CHECK(devices.size() == 0 && adapters.size() == 0);
    fail_create = false;
    CHECK(k->open_shared(nt, &r, &a) == S_OK && r == 702 && a == 1702);
    CHECK(k->resource_id(a) == 4321);
    k->close(r);
    k.reset();
    CHECK(devices.size() == 0 && adapters.size() == 0 && resources.size() == 0);
    std::puts("PASS sharing before binding, LUID failure, create failure cleanup and retry");

    missing_luid = true;
    k = KmtDevice::open();
    CHECK(k != nullptr);
    r = 0; a = 0;
    CHECK(k->open_shared(handle(91), &r, &a) == E_FAIL && r == 0 && a == 0);
    CHECK(k->share(42, GENERIC_ALL, &nt) == S_OK && nt == handle(91));
    k.reset();
    CHECK(devices.size() == 0 && adapters.size() == 0);
    std::puts("PASS optional LUID entry point and sharing");

    missing_luid = false;
    k = KmtDevice::open();
    vgd::viogpu::AllocationExchange surface{};
    fail_create = true;
    CHECK(k->share_surface(55, surface, &nt) == E_FAIL && nt == nullptr);
    CHECK(devices.empty() && adapters.empty());
    fail_create = false;
    CHECK(k->share_surface(55, surface, &nt) == S_OK && nt == handle(90));
    CHECK(resources.empty() && devices.size() == 1);
    CHECK(k->open_shared(nt, &r, &a) == S_OK && k->resource_id(a) == 4321);
    CHECK(k->open_shared(handle(91), &r2, &a2) == E_FAIL);
    CHECK(k->resource_id(a) == 4321);
    k->close(r);
    k.reset();
    CHECK(devices.empty() && adapters.empty() && resources.empty());

    k = KmtDevice::open();
    CHECK(k->open_shared(handle(91), &r, &a) == S_OK);
    CHECK(k->share_surface(55, surface, &nt) == S_OK && nt == handle(91));
    CHECK(k->resource_id(a) == 4321);
    k->close(r);
    k.reset();
    CHECK(devices.empty() && adapters.empty() && resources.empty());
    std::puts("PASS export-first binding, create recovery and import-first export lifetime");
}
