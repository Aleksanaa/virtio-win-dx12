// SPDX-License-Identifier: MIT
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace venera::umd {

// Loads the Venus ICD directly (no Vulkan loader): our build of yttrium's (icd/), venera_vulkan_virtio.dll next to this
// DLL. It can export memory, which present needs. Loaded once per process and kept.
PFN_vkGetInstanceProcAddr load_venus_icd();

} // namespace venera::umd
