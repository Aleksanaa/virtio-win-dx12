// SPDX-License-Identifier: MIT
// Guest probe: a Vulkan swapchain through the Vulkan loader (vulkan-1.dll), as applications use one. Creates a Win32
// surface and a swapchain on the first GPU, then for 120 frames acquires an image with a semaphore, clears it in a
// submission waiting on that semaphore and presents it. Prints every step's result.
//   vkswapchain [--d3d11] [--gpu N] [output file]
// --d3d11 first creates a D3D11 device (so our D3D11 UMD, and its DXVK on the same ICD, are in the process too);
// --gpu N uses the Nth physical device instead of the first.
#include <windows.h>
#include <d3d11.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

FILE* out = stdout;

#define CHECK(call)                                                                                                    \
    do {                                                                                                               \
        const VkResult r_ = (call);                                                                                    \
        std::fprintf(out, "%-60s %d\n", #call, static_cast<int>(r_));                                                 \
        if (r_ < 0)                                                                                                    \
            return 1;                                                                                                  \
    } while (0)

} // namespace

int main(int argc, char** argv)
{
    bool d3d11 = false;
    unsigned gpu_index = 0;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--d3d11")) {
            d3d11 = true;
        } else if (!std::strcmp(argv[i], "--gpu") && i + 1 < argc) {
            gpu_index = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (!(out = std::fopen(argv[i], "w"))) {
            std::fprintf(stderr, "cannot write %s\n", argv[i]);
            return 1;
        }
    }
    std::setvbuf(out, nullptr, _IONBF, 0);   // a crash keeps what was written before it
    ID3D11Device* d3d11_device = nullptr;
    if (d3d11) {
        D3D_FEATURE_LEVEL level{};
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &d3d11_device, &level, nullptr);
        std::fprintf(out, "D3D11CreateDevice: 0x%08lx, feature level 0x%x\n", hr, static_cast<unsigned>(level));
    }

    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    auto gipa = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                             reinterpret_cast<void*>(GetProcAddress(loader, "vkGetInstanceProcAddr")))
                       : nullptr;
    if (!gipa) {
        std::fprintf(out, "no vulkan-1.dll\n");
        return 1;
    }
#define INSTANCE_FN(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
    VkInstance instance = VK_NULL_HANDLE;
    INSTANCE_FN(vkCreateInstance);
    const char* instance_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instance_exts;
    CHECK(vkCreateInstance(&ici, nullptr, &instance));
    INSTANCE_FN(vkEnumeratePhysicalDevices);
    INSTANCE_FN(vkGetPhysicalDeviceProperties);
    INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceSupportKHR);
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    INSTANCE_FN(vkCreateWin32SurfaceKHR);
    INSTANCE_FN(vkCreateDevice);
    INSTANCE_FN(vkGetDeviceProcAddr);

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> gpus(count);
    CHECK(vkEnumeratePhysicalDevices(instance, &count, gpus.data()));
    for (VkPhysicalDevice g : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g, &p);
        std::fprintf(out, "gpu: %s (type %d)\n", p.deviceName, static_cast<int>(p.deviceType));
    }
    if (gpu_index >= count)
        return 1;
    VkPhysicalDevice gpu = gpus[gpu_index];
    std::fprintf(out, "using gpu %u\n", gpu_index);

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"vkswapchain";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(L"vkswapchain", L"vkswapchain", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 400,
                             nullptr, nullptr, wc.hInstance, nullptr);
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = wc.hInstance;
    sci.hwnd = wnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    CHECK(vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface));

    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count && family == UINT32_MAX; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present);
        if (present && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            family = i;
    }
    std::fprintf(out, "graphics+present queue family: %u\n", family);
    if (family == UINT32_MAX)
        return 1;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char* device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = device_exts;
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
#define DEVICE_FN(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))
    DEVICE_FN(vkGetDeviceQueue);
    DEVICE_FN(vkCreateSwapchainKHR);
    DEVICE_FN(vkGetSwapchainImagesKHR);
    DEVICE_FN(vkAcquireNextImageKHR);
    DEVICE_FN(vkQueuePresentKHR);
    DEVICE_FN(vkCreateSemaphore);
    DEVICE_FN(vkCreateFence);
    DEVICE_FN(vkWaitForFences);
    DEVICE_FN(vkResetFences);
    DEVICE_FN(vkCreateCommandPool);
    DEVICE_FN(vkAllocateCommandBuffers);
    DEVICE_FN(vkBeginCommandBuffer);
    DEVICE_FN(vkEndCommandBuffer);
    DEVICE_FN(vkResetCommandBuffer);
    DEVICE_FN(vkCmdPipelineBarrier);
    DEVICE_FN(vkCmdClearColorImage);
    DEVICE_FN(vkQueueSubmit);
    DEVICE_FN(vkDeviceWaitIdle);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps{};
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats.data());
    std::fprintf(out, "surface: %u formats, first %d, extent %ux%u, images %u-%u, usage 0x%x\n", count,
                 count ? static_cast<int>(formats[0].format) : -1, caps.currentExtent.width, caps.currentExtent.height,
                 caps.minImageCount, caps.maxImageCount, caps.supportedUsageFlags);
    if (!count)
        return 1;

    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = surface;
    swci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    swci.imageFormat = formats[0].format;
    swci.imageColorSpace = formats[0].colorSpace;
    swci.imageExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{640, 400};
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    CHECK(vkCreateSwapchainKHR(device, &swci, nullptr, &swapchain));
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr));
    std::vector<VkImage> images(count);
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()));
    std::fprintf(out, "swapchain: %u images\n", count);

    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    CHECK(vkCreateSemaphore(device, &semci, nullptr, &acquired));
    CHECK(vkCreateSemaphore(device, &semci, nullptr, &rendered));
    std::fprintf(out, "semaphores: acquired %p, rendered %p\n", reinterpret_cast<void*>(acquired),
                 reinterpret_cast<void*>(rendered));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence done = VK_NULL_HANDLE;
    CHECK(vkCreateFence(device, &fci, nullptr, &done));
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));

    unsigned presented = 0;
    for (unsigned frame = 0; frame < 120; frame++) {
        uint32_t index = 0;
        VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
        if (frame < 3 || r < 0)
            std::fprintf(out, "frame %u: acquire %d, image %u\n", frame, static_cast<int>(r), index);
        if (r < 0)
            break;
        vkWaitForFences(device, 1, &done, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &done);
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        const VkClearColorValue color = {{frame / 120.0f, 1.0f - frame / 120.0f, 0.5f, 1.0f}};
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &b.subresourceRange);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        vkEndCommandBuffer(cmd);
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered;
        r = vkQueueSubmit(queue, 1, &si, done);
        if (r < 0) {
            std::fprintf(out, "frame %u: submit %d\n", frame, static_cast<int>(r));
            break;
        }
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        r = vkQueuePresentKHR(queue, &pi);
        if (frame < 3 || r < 0)
            std::fprintf(out, "frame %u: present %d\n", frame, static_cast<int>(r));
        if (r < 0)
            break;
        presented++;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }
    vkDeviceWaitIdle(device);
    std::fprintf(out, "%u of 120 frames presented\nresult: %s\n", presented, presented == 120 ? "PASS" : "FAIL");
    DestroyWindow(wnd);
    return presented == 120 ? 0 : 1;
}
