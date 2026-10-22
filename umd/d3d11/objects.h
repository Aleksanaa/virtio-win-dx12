// SPDX-License-Identifier: MIT
// The D3D11 device behind the DDI and the objects behind its handles. Each DDI object lives in the runtime-allocated
// storage behind its handle (sized by the CalcPrivate*Size DDIs) and holds the DXVK object it forwards to.
#pragma once

#include "com.h"
#include "kmt.h"
#include "linear_image.h"
#include "log.h"
#include "vulkan_device.h"
#include "wdk.h"

#include <memory>
#include <new>
#include <vector>

namespace vgd::umd {

struct ResourceObject;

// Lives in the runtime-allocated storage behind D3D10DDI_HDEVICE (sized by CalcPrivateDeviceSize).
struct Device {
    D3D10DDI_HRTDEVICE rt_device{};
    D3D10DDI_HRTCORELAYER rt_core_layer{};
    D3DDDI_DEVICECALLBACKS kt{};                      // kernel thunks (allocations, contexts, present)
    D3D11DDI_CORELAYER_DEVICECALLBACKS um{};          // runtime callbacks in user mode
    const DXGI_DDI_BASE_CALLBACKS* dxgi = nullptr;    // pfnPresentCb
    Com<ID3D11Device5> d3d;
    Com<ID3D11DeviceContext4> ctx;
    VulkanDevice vulkan;                              // DXVK's device, for images on shared memory
    // The KMD understands viogpu::AllocationAlias (our patch): shared surfaces and primaries can be created.
    bool allocation_alias = false;
    std::unique_ptr<KmtDevice> kmt;                   // created on first use (kmt.h)
    HANDLE present_context = nullptr;                 // a kernel context of the runtime's, for presents
    HANDLE idle_event = nullptr;                      // set by DXVK when a flush completes (wait_idle)
    // Output-merger bindings, rebound when RotateResourceIdentities swaps what they view.
    std::vector<ID3D11RenderTargetView*> om_rtvs;
    ID3D11DepthStencilView* om_dsv = nullptr;

    static Device* from(D3D10DDI_HDEVICE h) { return static_cast<Device*>(h.pDrvPrivate); }
    static Device* from(DXGI_DDI_HDEVICE h) { return reinterpret_cast<Device*>(h); }
    // Void DDIs report failure through the runtime's error callback (which may remove the device: logged).
    void set_error(HRESULT hr) const
    {
        if (hr != DXGI_DDI_ERR_WASSTILLDRAWING)
            VGD_WARN("SetErrorCb(0x%08lx)", hr);
        um.pfnSetErrorCb(rt_core_layer, hr);
    }
    KmtDevice* kernel()
    {
        if (!kmt)
            kmt = KmtDevice::open();
        return kmt.get();
    }
    // Waits until the GPU has done all work submitted so far.
    HRESULT wait_idle();
};

template <class T, class H>
T* object(H h)
{
    return static_cast<T*>(h.pDrvPrivate);
}

struct ViewObject;

struct ResourceObject {
    Com<ID3D11Resource> resource;
    D3D10DDIRESOURCE_TYPE dimension = D3D10DDIRESOURCE_BUFFER;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bind = 0;   // D3D10_DDI_RESOURCE_BIND_FLAG
    UINT misc = 0;   // D3D10_DDI_RESOURCE_MISC_FLAG
    UINT cpu = 0;    // D3D10_DDI_CPU_ACCESS
    UINT width = 0, height = 0, mips = 1, array_size = 1, samples = 1;
    // On memory shared with other processes (linear_image.h): swapchain buffers, primaries, shared surfaces.
    std::unique_ptr<LinearImage> image;
    // Its runtime allocation (ours to deallocate if `allocated`), and the runtime resource handle.
    D3DKMT_HANDLE allocation = 0;
    D3D10DDI_HRTRESOURCE rt_resource{};
    bool allocated = false;
    // Views of it, recreated when RotateResourceIdentities swaps its storage (present buffers only).
    std::vector<ViewObject*> views;

    static ResourceObject* from(D3D10DDI_HRESOURCE h) { return object<ResourceObject>(h); }
    static ResourceObject* from(DXGI_DDI_HRESOURCE h) { return reinterpret_cast<ResourceObject*>(h); }
};

// The COM resource behind a handle; null for a null handle.
inline ID3D11Resource* resource_of(D3D10DDI_HRESOURCE h)
{
    return h.pDrvPrivate ? ResourceObject::from(h)->resource.get() : nullptr;
}
inline ID3D11Buffer* buffer_of(D3D10DDI_HRESOURCE h)
{
    return static_cast<ID3D11Buffer*>(resource_of(h));
}

// Shader resource, render target, depth/stencil and unordered access views: the view and what made it, so it can be
// made again over new storage.
struct ViewObject {
    enum Kind { srv, rtv, dsv, uav } kind;
    ResourceObject* resource = nullptr;
    Com<ID3D11View> view;
    union {
        D3D11_SHADER_RESOURCE_VIEW_DESC1 srv;
        D3D11_RENDER_TARGET_VIEW_DESC1 rtv;
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv;
        D3D11_UNORDERED_ACCESS_VIEW_DESC1 uav;
    } desc{};

    explicit ViewObject(Kind k) : kind(k) {}
    ~ViewObject();
    HRESULT create(ID3D11Device5* device);

    template <class T>
    T* as() const
    {
        return static_cast<T*>(view.get());
    }
};

template <class T, class H>
T* view_of(H h)
{
    return h.pDrvPrivate ? object<ViewObject>(h)->template as<T>() : nullptr;
}

struct ShaderObject {
    Com<ID3D11DeviceChild> shader;
};

template <class T>
T* shader_of(D3D10DDI_HSHADER h)
{
    return h.pDrvPrivate ? static_cast<T*>(object<ShaderObject>(h)->shader.get()) : nullptr;
}

template <class T>
struct StateObject {
    Com<T> state;
};

template <class T, class H>
T* state_of(H h)
{
    return h.pDrvPrivate ? object<StateObject<T>>(h)->state.get() : nullptr;
}

struct QueryObject {
    D3D10DDI_QUERY type = D3D10DDI_QUERY_EVENT;
    Com<ID3D11Query> query;   // or ID3D11Predicate
};

// --- Fill functions of the DDI tables, by source ------------------------------------------------------------------

void fill_resource_ddi(D3D11_1DDI_DEVICEFUNCS& t);   // resource.cpp
void fill_view_ddi(D3D11_1DDI_DEVICEFUNCS& t);       // view.cpp
void fill_pipeline_ddi(D3D11_1DDI_DEVICEFUNCS& t);   // pipeline.cpp
void fill_draw_ddi(D3D11_1DDI_DEVICEFUNCS& t);       // draw.cpp
void fill_query_ddi(D3D11_1DDI_DEVICEFUNCS& t);      // query.cpp
void fill_dxgi_ddi(DXGI1_2_DDI_BASE_FUNCTIONS& t);   // present.cpp

// Creating shared surfaces and primaries, and opening those of other processes (present.cpp).
bool is_shared_surface(const D3D11DDIARG_CREATERESOURCE& args);
HRESULT create_shared_surface(Device& d, const D3D11DDIARG_CREATERESOURCE& args, D3D10DDI_HRTRESOURCE rt,
                              ResourceObject& out);
HRESULT open_shared_surface(Device& d, const D3D10DDIARG_OPENRESOURCE& args, ResourceObject& out);
void destroy_shared_surface(Device& d, ResourceObject& r);

// Format answers (format.cpp).
void APIENTRY check_format_support(D3D10DDI_HDEVICE h, DXGI_FORMAT format, UINT* caps);
void APIENTRY check_multisample_quality_levels(D3D10DDI_HDEVICE h, DXGI_FORMAT format, UINT samples, UINT* levels);

// Translations of DDI values that differ from the API's (resource.cpp).
UINT api_bind_flags(UINT ddi_bind);
D3D11_BOX api_box(const D3D10_DDI_BOX& b);

} // namespace vgd::umd
