// SPDX-License-Identifier: MIT
// A kernel device of our own on the viogpu adapter. The runtimes do not give a UMD their device's kernel handle, and
// some operations on shared memory need one: opening the NT handle our Venus ICD exports (to alias a runtime
// allocation to its host resource) and sharing a runtime allocation as an NT handle (to import it into Vulkan).
#pragma once

#include <windows.h>

#include <d3dkmthk.h>

#include <memory>

namespace vgd::umd {

class KmtDevice {
public:
    // The viogpu adapter's device, or null (logged) if there is none or gdi32 lacks the entry points.
    static std::unique_ptr<KmtDevice> open();
    ~KmtDevice();

    // Opens a shared NT handle of one allocation: `resource` holds it open until close().
    HRESULT open_shared(HANDLE nt, D3DKMT_HANDLE* resource, D3DKMT_HANDLE* allocation);
    void close(D3DKMT_HANDLE resource);
    // An NT handle of the runtime resource `resource` (a kernel resource handle of this process); the caller owns it.
    HRESULT share(D3DKMT_HANDLE resource, HANDLE* nt) { return share(resource, SHARED_ALLOCATION_ALL_ACCESS, nt); }
    // Of any shareable kernel object of this process (resources, sync objects), with `access`.
    HRESULT share(D3DKMT_HANDLE object, DWORD access, HANDLE* nt);
    // The host (virtio-gpu) resource id that `allocation`, an allocation of this device, names; 0 if the KMD says none.
    UINT resource_id(D3DKMT_HANDLE allocation);

private:
    KmtDevice() = default;

    PFND3DKMT_ENUMADAPTERS2 enum_adapters_ = nullptr;
    PFND3DKMT_QUERYADAPTERINFO query_adapter_info_ = nullptr;
    PFND3DKMT_CLOSEADAPTER close_adapter_ = nullptr;
    PFND3DKMT_CREATEDEVICE create_device_ = nullptr;
    PFND3DKMT_DESTROYDEVICE destroy_device_ = nullptr;
    PFND3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query_resource_info_ = nullptr;
    PFND3DKMT_OPENRESOURCEFROMNTHANDLE open_resource_ = nullptr;
    PFND3DKMT_DESTROYALLOCATION destroy_allocation_ = nullptr;
    PFND3DKMT_SHAREOBJECTS share_objects_ = nullptr;
    PFND3DKMT_ESCAPE escape_ = nullptr;
    D3DKMT_HANDLE adapter_ = 0;
    D3DKMT_HANDLE device_ = 0;
};

// Our Venus ICD build exports memory as the NT handle value in place of an fd (icd/patches/0003): the CRT cannot wrap
// a shared graphics resource handle in a descriptor. These convert between the two.
inline HANDLE handle_of_fd(int fd)
{
    return fd < 0 ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd));
}
// -1 if the handle value does not fit an fd.
inline int fd_of_handle(HANDLE h)
{
    const auto v = reinterpret_cast<uintptr_t>(h);
    return v > INT_MAX ? -1 : static_cast<int>(v);
}

} // namespace vgd::umd
