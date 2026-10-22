// SPDX-License-Identifier: MIT
#include "shared_image.h"
#include "log.h"
#include "vkd3d_bridge.h"

namespace vgd::umd {

SharedImage::~SharedImage() = default;

HRESULT SharedImage::describe(const D3D12_RESOURCE_DESC& desc, const DXGI_FORMAT* castable, UINT castable_count,
                              LinearImageDesc* out)
{
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels > 1 || desc.DepthOrArraySize > 1 ||
        desc.SampleDesc.Count > 1 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return E_NOTIMPL;
    if (!desc.Width || desc.Width > UINT32_MAX)
        return E_INVALIDARG;
    out->format = vgd_vkd3d_vk_format(desc.Format);
    for (UINT i = 0; i < castable_count; i++)
        out->view_formats.push_back(vgd_vkd3d_vk_format(castable[i]));
    out->width = static_cast<uint32_t>(desc.Width);
    out->height = desc.Height ? desc.Height : 1;
    out->usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        out->usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
        out->usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        out->usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    return S_OK;
}

// The image lent to vkd3d-proton, in the layout vkd3d-proton keeps it in.
HRESULT SharedImage::lend(const D3D12_RESOURCE_DESC& desc)
{
    Engine& e = *engine_;
    D3D12_RESOURCE_DESC1 desc1{};
    desc1.Dimension = desc.Dimension;
    desc1.Width = desc.Width;
    desc1.Height = desc.Height;
    desc1.DepthOrArraySize = 1;
    desc1.MipLevels = 1;
    desc1.Format = desc.Format;
    desc1.SampleDesc = {1, 0};
    desc1.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc1.Flags = desc.Flags;
    HRESULT hr = vgd_vkd3d_borrow_image(e.device.get(), &desc1, image_->image(), resource_.put_void());
    if (FAILED(hr)) {
        VGD_WARN("shared image: vkd3d-proton did not take the image: 0x%08lx", hr);
        return hr;
    }
    VkImageLayout target = VK_IMAGE_LAYOUT_GENERAL;
    vgd_vkd3d_image_layout(e.device.get(), resource_.get(), D3D12_RESOURCE_STATE_COMMON, &target);
    // An imported image keeps what the exporter wrote only in practice: external images must start UNDEFINED, and
    // the spec's way to keep contents (an acquire from VK_QUEUE_FAMILY_EXTERNAL in the layout the exporter
    // released) needs a release the exporters do not make. Drivers leave linear memory alone in this transition.
    const VkImage image = image_->image();
    return e.run([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = target;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        e.vulkan.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                         0, nullptr, 0, nullptr, 1, &b);
    });
}

HRESULT SharedImage::create_exported(const std::shared_ptr<Engine>& engine, const D3D12_RESOURCE_DESC& desc,
                                     const DXGI_FORMAT* castable, UINT castable_count,
                                     std::unique_ptr<SharedImage>* out, int* fd)
{
    *fd = -1;
    LinearImageDesc ld;
    HRESULT hr = describe(desc, castable, castable_count, &ld);
    if (FAILED(hr))
        return hr;
    std::unique_ptr<SharedImage> s(new SharedImage);
    s->engine_ = engine;
    int exported = -1;
    hr = LinearImage::create_exported(engine->vulkan, ld, &s->image_, &exported);
    if (SUCCEEDED(hr))
        hr = s->lend(desc);
    if (FAILED(hr)) {
        if (exported >= 0)
            CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(exported)));
        return hr;
    }
    *fd = exported;
    *out = std::move(s);
    return S_OK;
}

HRESULT SharedImage::create_imported(const std::shared_ptr<Engine>& engine, const D3D12_RESOURCE_DESC& desc,
                                     const DXGI_FORMAT* castable, UINT castable_count, int fd, uint64_t size,
                                     std::unique_ptr<SharedImage>* out, bool* consumed)
{
    *consumed = false;
    LinearImageDesc ld;
    HRESULT hr = describe(desc, castable, castable_count, &ld);
    if (FAILED(hr))
        return hr;
    std::unique_ptr<SharedImage> s(new SharedImage);
    s->engine_ = engine;
    hr = LinearImage::create_imported(engine->vulkan, ld, fd, size, &s->image_, consumed);
    if (SUCCEEDED(hr))
        hr = s->lend(desc);
    if (FAILED(hr))
        return hr;
    *out = std::move(s);
    return S_OK;
}

} // namespace vgd::umd
