/* SPDX-License-Identifier: MIT */
/* vkd3d-proton's own entry points and extension interfaces, for the C++ side (vkd3d_bridge.c). COM objects cross as
 * void*: ID3D12Device, ID3D12Resource, ID3D12CommandQueue and ID3DBlob, the same objects under either header set. */
#pragma once

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An ID3D12Device on the Venus ICD behind `gipa`, on its first non-CPU physical device (whose name goes to `name`).
 * The device is per process: later calls return the same one. */
HRESULT vgd_vkd3d_create_device(PFN_vkGetInstanceProcAddr gipa, const char *const *optional_extensions,
        uint32_t optional_extension_count, void **device, char *name, size_t name_size);
HRESULT vgd_vkd3d_vulkan_handles(void *device, VkInstance *instance, VkPhysicalDevice *physical, VkDevice *vk_device);
/* An ID3D12Resource on a VkImage of `device`'s VkDevice (a D3D12_RESOURCE_DESC1, 2D textures only), which the caller
 * keeps owning: the image and its memory must outlive the resource. Its state is COMMON. */
HRESULT vgd_vkd3d_borrow_image(void *device, const void *desc1, VkImage image, void **resource);
/* The VkImageLayout vkd3d-proton keeps `resource` in while it is in D3D12 state `state`. */
HRESULT vgd_vkd3d_image_layout(void *device, void *resource, uint32_t state, VkImageLayout *layout);
/* Exclusive use of `queue`'s VkQueue, until vgd_vkd3d_unlock_queue. */
HRESULT vgd_vkd3d_lock_queue(void *device, void *queue, VkQueue *vk_queue, uint32_t *family);
void vgd_vkd3d_unlock_queue(void *device, void *queue);
/* A D3D12_VERSIONED_ROOT_SIGNATURE_DESC serialized as CreateRootSignature takes it, as an ID3DBlob. */
HRESULT vgd_vkd3d_serialize_root_signature(const void *versioned_desc, void **blob);
/* The VkFormat vkd3d-proton stores a DXGI format as; VK_FORMAT_UNDEFINED if none. */
VkFormat vgd_vkd3d_vk_format(uint32_t dxgi_format);

#ifdef __cplusplus
}
#endif
