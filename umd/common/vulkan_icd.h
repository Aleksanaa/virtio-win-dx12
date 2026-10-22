// SPDX-License-Identifier: MIT
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace vgd::umd {

// Loads the Venus ICD directly (no Vulkan loader): our build of yttrium's (icd/), vgd_vulkan_virtio.dll next to this
// DLL, or else the driver registered under HKLM\SOFTWARE\Khronos\Vulkan\Drivers whose manifest names virtio.
// Loaded once per process and kept.
PFN_vkGetInstanceProcAddr load_venus_icd();

} // namespace vgd::umd
