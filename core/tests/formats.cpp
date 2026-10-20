// SPDX-License-Identifier: MIT
// Format table sanity: mappings for the common formats, and the features every D3D12-capable GPU has for them.
#include "test_util.h"

#include <vgd/formats.h>

static bool formats(vgd::Device& dev)
{
    CHECK(vgd::vk_format(DXGI_FORMAT_R8G8B8A8_UNORM) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(vgd::vk_format(DXGI_FORMAT_B5G6R5_UNORM) == VK_FORMAT_R5G6B5_UNORM_PACK16);
    CHECK(vgd::vk_format(DXGI_FORMAT_R10G10B10A2_UNORM) == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    CHECK(vgd::vk_format(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) == VK_FORMAT_UNDEFINED);
    CHECK(vgd::is_typeless(DXGI_FORMAT_R8G8B8A8_TYPELESS) && !vgd::is_typeless(DXGI_FORMAT_R8G8B8A8_UNORM));

    auto rgba8 = dev.format_features(DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(rgba8.optimal & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    CHECK(rgba8.optimal & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT);
    CHECK(rgba8.buffer & VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT);
    CHECK(rgba8.samples & VK_SAMPLE_COUNT_4_BIT);   // FL 11_0 requires 4x MSAA on RGBA8

    // D24S8 is required by every feature level; devices without it get D32S8 behind the same DXGI format.
    auto d24 = dev.format_features(DXGI_FORMAT_D24_UNORM_S8_UINT);
    CHECK(d24.optimal & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT);
    CHECK(d24.samples & VK_SAMPLE_COUNT_4_BIT);

    auto d32 = dev.format_features(DXGI_FORMAT_D32_FLOAT);
    CHECK(d32.optimal & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT);

    auto r32u = dev.format_features(DXGI_FORMAT_R32_UINT);
    CHECK(r32u.optimal & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT);
    CHECK(r32u.buffer & VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT);

    auto none = dev.format_features(DXGI_FORMAT_R1_UNORM);
    CHECK(!none.optimal && !none.buffer);
    return true;
}

int main() { return vgd_test::run_on_adapters("formats", formats); }
