// SPDX-License-Identifier: MIT
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
static void *load(const char *p) { return LoadLibraryExA(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH); }
static void *symbol(void *m, const char *n)
{
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(m), n));
}
static void unload(void *m) { FreeLibrary(static_cast<HMODULE>(m)); }
#else
#include <dlfcn.h>
static void *load(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void *symbol(void *m, const char *n) { return dlsym(m, n); }
static void unload(void *m) { dlclose(m); }
#endif
static uint64_t now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct AllocationHeader {
    void *raw;
    size_t size;
};
struct FailureAllocator {
    std::atomic<unsigned> live{0};
    std::atomic<bool> rejected{false};
};
static VKAPI_ATTR void *VKAPI_CALL allocate(void *user, size_t size, size_t alignment,
                                            VkSystemAllocationScope scope)
{
    auto *state = static_cast<FailureAllocator *>(user);
    // Venus allocates its queue-family array after native device creation.
    if (size == sizeof(uint32_t) && scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE) {
        state->rejected = true;
        return nullptr;
    }
    if (alignment < alignof(AllocationHeader))
        alignment = alignof(AllocationHeader);
    if (size > SIZE_MAX - alignment - sizeof(AllocationHeader))
        return nullptr;
    void *raw = std::malloc(size + alignment + sizeof(AllocationHeader));
    if (!raw)
        return nullptr;
    auto ptr = (reinterpret_cast<uintptr_t>(raw) + sizeof(AllocationHeader) + alignment - 1) &
               ~(static_cast<uintptr_t>(alignment) - 1);
    auto *header = reinterpret_cast<AllocationHeader *>(ptr) - 1;
    *header = {raw, size};
    ++state->live;
    return reinterpret_cast<void *>(ptr);
}
static VKAPI_ATTR void VKAPI_CALL release(void *user, void *ptr)
{
    if (ptr) {
        auto *header = static_cast<AllocationHeader *>(ptr) - 1;
        std::free(header->raw);
        --static_cast<FailureAllocator *>(user)->live;
    }
}
static VKAPI_ATTR void *VKAPI_CALL reallocate(void *user, void *original, size_t size, size_t alignment,
                                              VkSystemAllocationScope scope)
{
    if (!size) {
        release(user, original);
        return nullptr;
    }
    void *ptr = allocate(user, size, alignment, scope);
    if (ptr && original) {
        size_t old_size = (static_cast<AllocationHeader *>(original) - 1)->size;
        std::memcpy(ptr, original, size < old_size ? size : old_size);
        release(user, original);
    }
    return ptr;
}
#define CHECK(call)                                                                                          \
    do {                                                                                                     \
        VkResult r = (call);                                                                                 \
        if (r != VK_SUCCESS) {                                                                               \
            std::printf("FAIL %s: %d\n", #call, r);                                                          \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)
int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3) {
        std::printf("Usage: vkdeviceprobe <loader or ICD> <control|destroy|paced|recycle|create-failure>\n");
        return 2;
    }
    const bool control = std::strcmp(argv[2], "control") == 0;
    const bool paced = std::strcmp(argv[2], "paced") == 0;
    const bool recycle = std::strcmp(argv[2], "recycle") == 0;
    const bool failure = std::strcmp(argv[2], "create-failure") == 0;
    const bool early_destroy = !control && !failure;
    if (!control && !paced && !recycle && !failure && std::strcmp(argv[2], "destroy") != 0)
        return 2;
    void *module = load(argv[1]);
    if (!module) {
        std::printf("FAIL load\n");
        return 1;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(symbol(module, "vkGetInstanceProcAddr"));
    if (!gipa)
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(symbol(module, "vk_icdGetInstanceProcAddr"));
    if (!gipa) {
        std::printf("FAIL no Vulkan entry point\n");
        unload(module);
        return 1;
    }
    auto negotiate = reinterpret_cast<VkResult(VKAPI_PTR *)(uint32_t *)>(
        symbol(module, "vk_icdNegotiateLoaderICDInterfaceVersion"));
    uint32_t icd_interface = 5;
    if (negotiate)
        CHECK(negotiate(&icd_interface));
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(create_instance(&ici, nullptr, &instance));
#define FN(f) auto f = reinterpret_cast<PFN_##f>(gipa(instance, #f))
    FN(vkEnumeratePhysicalDevices);
    FN(vkGetPhysicalDeviceProperties);
    FN(vkGetPhysicalDeviceQueueFamilyProperties);
    FN(vkGetPhysicalDeviceFeatures2);
    FN(vkCreateDevice);
    FN(vkGetDeviceQueue);
    FN(vkCreateSemaphore);
    FN(vkSignalSemaphore);
    FN(vkQueueSubmit);
    FN(vkCreateFence);
    FN(vkWaitForFences);
    FN(vkDeviceWaitIdle);
    FN(vkDestroyFence);
    FN(vkDestroySemaphore);
    FN(vkDestroyDevice);
    FN(vkDestroyInstance);
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    for (auto d : devices) {
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            physical = d;
            break;
        }
    }
    if (!physical) {
        std::printf("FAIL no hardware device\n");
        return 1;
    }
    std::printf("GPU %s mode %s\n", props.deviceName, argv[2]);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        family++;
    if (family == count)
        return 1;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &timeline;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!timeline.timelineSemaphore)
        return 1;
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &timeline;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice first, second;
    CHECK(vkCreateDevice(physical, &dci, nullptr, &first));
    CHECK(vkCreateDevice(physical, &dci, nullptr, &second));
    VkQueue qa, qb;
    vkGetDeviceQueue(first, family, 0, &qa);
    vkGetDeviceQueue(second, family, 0, &qb);
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &type;
    VkSemaphore semaphore;
    CHECK(vkCreateSemaphore(first, &sci, nullptr, &semaphore));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fa, fb;
    CHECK(vkCreateFence(first, &fci, nullptr, &fa));
    CHECK(vkCreateFence(second, &fci, nullptr, &fb));
    std::atomic<bool> done{false};
    bool watchdog = false;
    VkResult signal_result = VK_NOT_READY;
    std::jthread rescue([&] {
        for (unsigned i = 0; i < 200 && !done; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        watchdog = !done;
        std::printf("CPU signal start, watchdog %u\n", watchdog ? 1u : 0u);
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signal.semaphore = semaphore;
        signal.value = 1;
        signal_result = vkSignalSemaphore(first, &signal);
        std::printf("CPU signal result %d\n", signal_result);
    });
    const uint64_t one = 1;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    wait.waitSemaphoreValueCount = 1;
    wait.pWaitSemaphoreValues = &one;
    VkSubmitInfo a{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    a.pNext = &wait;
    a.waitSemaphoreCount = 1;
    a.pWaitSemaphores = &semaphore;
    a.pWaitDstStageMask = &stage;
    for (unsigned i = 0; i < (paced ? 128u : 1u); i++) {
        CHECK(vkQueueSubmit(qa, 1, &a, VK_NULL_HANDLE));
        if (paced)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("A waits queued: %u\n", paced ? 128u : 1u);
    CHECK(vkQueueSubmit(qb, 0, nullptr, fb));
    CHECK(vkWaitForFences(second, 1, &fb, VK_TRUE, 5000000000ull));
    CHECK(vkDeviceWaitIdle(second));
    vkDestroyFence(second, fb, nullptr);
    std::printf("B idle\n");
    const auto start = now();
    if (early_destroy) {
        std::printf("B destroy start\n");
        vkDestroyDevice(second, nullptr);
        std::printf("B destroy returned after %llu ms\n", (unsigned long long)(now() - start));
    }
    if (failure) {
        FailureAllocator state;
        VkAllocationCallbacks allocator{};
        allocator.pUserData = &state;
        allocator.pfnAllocation = allocate;
        allocator.pfnReallocation = reallocate;
        allocator.pfnFree = release;
        VkDevice extra = VK_NULL_HANDLE;
        VkResult result = vkCreateDevice(physical, &dci, &allocator, &extra);
        if (result == VK_SUCCESS)
            vkDestroyDevice(extra, &allocator);
        std::printf("Create failure result %d, injected %u, live allocations %u\n", result,
                    state.rejected ? 1u : 0u, state.live.load());
        if (result != VK_ERROR_OUT_OF_HOST_MEMORY || !state.rejected || state.live)
            return 1;
    }
    done = true;
    rescue.join();
    if (signal_result != VK_SUCCESS)
        return 1;
    CHECK(vkQueueSubmit(qa, 0, nullptr, fa));
    CHECK(vkWaitForFences(first, 1, &fa, VK_TRUE, 5000000000ull));
    CHECK(vkDeviceWaitIdle(first));
    std::printf("A completion passed\n");
    if (recycle) {
        for (unsigned i = 0; i < 80; i++) {
            VkDevice extra;
            CHECK(vkCreateDevice(physical, &dci, nullptr, &extra));
            VkQueue queue;
            vkGetDeviceQueue(extra, family, 0, &queue);
            VkFence fence;
            CHECK(vkCreateFence(extra, &fci, nullptr, &fence));
            CHECK(vkQueueSubmit(queue, 0, nullptr, fence));
            CHECK(vkWaitForFences(extra, 1, &fence, VK_TRUE, 5000000000ull));
            CHECK(vkDeviceWaitIdle(extra));
            vkDestroyFence(extra, fence, nullptr);
            vkDestroyDevice(extra, nullptr);
        }
        std::printf("80 device lifetimes passed\n");
    }
    vkDestroyFence(first, fa, nullptr);
    vkDestroySemaphore(first, semaphore, nullptr);
    vkDestroyDevice(first, nullptr);
    if (!early_destroy)
        vkDestroyDevice(second, nullptr);
    vkDestroyInstance(instance, nullptr);
    unload(module);
    std::printf("%s CPU progress\n", watchdog ? "FAIL" : "PASS");
    return watchdog ? 1 : 0;
}
