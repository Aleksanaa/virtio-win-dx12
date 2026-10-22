/* SPDX-License-Identifier: MIT */
/* The one translation unit that sees vkd3d-proton's headers. Its D3D12 headers are generated from its own IDL and
 * clash with Microsoft's, which the rest of the UMD uses; the COM objects are the same either way, so everything
 * crosses this boundary as plain pointers (see vkd3d_bridge.h). */
#include <vkd3d.h>
#include <vkd3d_device_vkd3d_ext.h>

#include "vkd3d_bridge.h"

#include <stdio.h>
#include <string.h>

/* Venus exposes the host's GPU and, on hosts with Mesa, llvmpipe: prefer anything that is not the CPU. */
static VkPhysicalDevice pick_physical_device(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, char *name,
        size_t name_size)
{
    PFN_vkEnumeratePhysicalDevices enumerate =
            (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties =
            (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    VkPhysicalDevice devices[8], chosen = VK_NULL_HANDLE;
    uint32_t count = 8, i;

    if (!enumerate || !properties || enumerate(instance, &count, devices) < 0)
        return VK_NULL_HANDLE;
    for (i = 0; i < count; ++i)
    {
        VkPhysicalDeviceProperties p;
        properties(devices[i], &p);
        if (!chosen || p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
        {
            chosen = devices[i];
            snprintf(name, name_size, "%s", p.deviceName);
            if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
                break;
        }
    }
    return chosen;
}

HRESULT vgd_vkd3d_create_device(PFN_vkGetInstanceProcAddr gipa, const char *const *optional_extensions,
        uint32_t optional_extension_count, void **device, char *name, size_t name_size)
{
    struct vkd3d_instance_create_info instance_info;
    struct vkd3d_device_create_info device_info;
    struct vkd3d_instance *instance;
    HRESULT hr;

    *device = NULL;
    if (name_size)
        name[0] = 0;

    memset(&instance_info, 0, sizeof(instance_info));
    instance_info.pfn_vkGetInstanceProcAddr = gipa;
    if (FAILED(hr = vkd3d_create_instance(&instance_info, &instance)))
        return hr;

    memset(&device_info, 0, sizeof(device_info));
    device_info.minimum_feature_level = D3D_FEATURE_LEVEL_11_0;
    device_info.instance = instance;
    device_info.vk_physical_device = pick_physical_device(gipa, vkd3d_instance_get_vk_instance(instance), name,
            name_size);
    device_info.optional_device_extensions = optional_extensions;
    device_info.optional_device_extension_count = optional_extension_count;
    /* One device per process and adapter, shared by every DDI device, as D3D12CreateDevice does. */
    device_info.independent = false;

    hr = device_info.vk_physical_device
            ? vkd3d_create_device(&device_info, &IID_ID3D12Device, device) : DXGI_ERROR_UNSUPPORTED;
    vkd3d_instance_decref(instance);
    return hr;
}

HRESULT vgd_vkd3d_vulkan_handles(void *device, VkInstance *instance, VkPhysicalDevice *physical, VkDevice *vk_device)
{
    ID3D12DeviceExt *ext;
    HRESULT hr;

    if (FAILED(hr = ID3D12Device_QueryInterface((ID3D12Device *)device, &IID_ID3D12DeviceExt, (void **)&ext)))
        return hr;
    hr = ID3D12DeviceExt_GetVulkanHandles(ext, instance, physical, vk_device);
    ID3D12DeviceExt_Release(ext);
    return hr;
}

HRESULT vgd_vkd3d_borrow_image(void *device, const void *desc1, VkImage image, void **resource)
{
    ID3D12DeviceExt1 *ext;
    HRESULT hr;

    *resource = NULL;
    if (FAILED(hr = ID3D12Device_QueryInterface((ID3D12Device *)device, &IID_ID3D12DeviceExt1, (void **)&ext)))
        return hr;
    hr = ID3D12DeviceExt1_CreateResourceFromBorrowedHandle(ext, (const D3D12_RESOURCE_DESC1 *)desc1,
            (UINT64)image, (ID3D12Resource **)resource);
    ID3D12DeviceExt1_Release(ext);
    return hr;
}

static HRESULT interop(void *device, ID3D12DXVKInteropDevice **out)
{
    return ID3D12Device_QueryInterface((ID3D12Device *)device, &IID_ID3D12DXVKInteropDevice, (void **)out);
}

HRESULT vgd_vkd3d_image_layout(void *device, void *resource, uint32_t state, VkImageLayout *layout)
{
    ID3D12DXVKInteropDevice *i;
    HRESULT hr;

    if (FAILED(hr = interop(device, &i)))
        return hr;
    ID3D12DXVKInteropDevice_GetVulkanImageLayout(i, (ID3D12Resource *)resource, (D3D12_RESOURCE_STATES)state, layout);
    ID3D12DXVKInteropDevice_Release(i);
    return S_OK;
}

HRESULT vgd_vkd3d_lock_queue(void *device, void *queue, VkQueue *vk_queue, uint32_t *family)
{
    ID3D12DXVKInteropDevice *i;
    HRESULT hr;

    if (FAILED(hr = interop(device, &i)))
        return hr;
    if (SUCCEEDED(hr = ID3D12DXVKInteropDevice_GetVulkanQueueInfo(i, (ID3D12CommandQueue *)queue, vk_queue, family)))
        hr = ID3D12DXVKInteropDevice_LockCommandQueue(i, (ID3D12CommandQueue *)queue);
    ID3D12DXVKInteropDevice_Release(i);
    return hr;
}

void vgd_vkd3d_unlock_queue(void *device, void *queue)
{
    ID3D12DXVKInteropDevice *i;

    if (FAILED(interop(device, &i)))
        return;
    ID3D12DXVKInteropDevice_UnlockCommandQueue(i, (ID3D12CommandQueue *)queue);
    ID3D12DXVKInteropDevice_Release(i);
}

HRESULT vgd_vkd3d_serialize_root_signature(const void *versioned_desc, void **blob)
{
    ID3DBlob *error = NULL;
    HRESULT hr;

    *blob = NULL;
    hr = vkd3d_serialize_versioned_root_signature((const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *)versioned_desc,
            (ID3DBlob **)blob, &error);
    if (error)
        ID3D10Blob_Release(error);
    return hr;
}

VkFormat vgd_vkd3d_vk_format(uint32_t dxgi_format)
{
    return vkd3d_get_vk_format((DXGI_FORMAT)dxgi_format);
}

/* vkd3d-proton's test-suite controls, which its own D3D12Core.dll (libs/d3d12core) defines: an embedder answers
 * them for a process that is not under its tests. */
bool vkd3d_debug_control_is_test_suite(void)
{
    return false;
}

bool vkd3d_debug_control_explode_on_vvl_error(void)
{
    return false;
}

bool vkd3d_debug_control_has_out_of_spec_test_behavior(VKD3D_DEBUG_CONTROL_OUT_OF_SPEC_BEHAVIOR behavior)
{
    return false;
}

VKD3D_DEBUG_CONTROL_BEHAVIOR_FLAGS vkd3d_debug_control_get_behavior_flags(void)
{
    return 0;
}

bool vkd3d_debug_control_mute_message_id(const char *vuid)
{
    return false;
}
