// SPDX-License-Identifier: MIT
// Minimal harness for core tests on the Linux host: loads the system Vulkan loader and runs a test body
// on every usable adapter (or those matching VGD_ADAPTER, a substring of the device name).
#pragma once

#include <vgd/core.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            return false;                                                                            \
        }                                                                                            \
    } while (0)

#define CHECK_HR(expr)                                                                               \
    do {                                                                                             \
        HRESULT hr_ = (expr);                                                                        \
        if (FAILED(hr_)) {                                                                           \
            std::fprintf(stderr, "  FAIL %s:%d: %s = 0x%08x\n", __FILE__, __LINE__, #expr,           \
                         static_cast<unsigned>(hr_));                                                \
            return false;                                                                            \
        }                                                                                            \
    } while (0)

namespace vgd_test {

inline void log_sink(vgd::LogLevel level, const char* msg)
{
    static const char* names[] = {"error", "warn", "info", "debug"};
    if (level <= vgd::LogLevel::info || std::getenv("VGD_DEBUG"))
        std::fprintf(stderr, "  [%s] %s\n", names[static_cast<int>(level)], msg);
}

inline int run_on_adapters(const char* test_name, const std::function<bool(vgd::Device&)>& body)
{
    vgd::set_log_sink(log_sink);
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "%s: SKIP (no libvulkan.so.1)\n", test_name);
        return 77;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    std::unique_ptr<vgd::Instance> instance;
    if (FAILED(vgd::Instance::create(gipa, &instance))) {
        std::fprintf(stderr, "%s: FAIL (instance)\n", test_name);
        return 1;
    }
    const char* filter = std::getenv("VGD_ADAPTER");
    int ran = 0, failed = 0;
    const auto& adapters = instance->adapters();
    for (size_t i = 0; i < adapters.size(); i++) {
        const auto& a = adapters[i];
        if (filter && !std::strstr(a.name.c_str(), filter))
            continue;
        if (!a.usable()) {
            std::string missing;
            for (const auto& m : a.missing)
                missing += " " + m;
            std::fprintf(stderr, "%s [%s]: SKIP, missing:%s\n", test_name, a.name.c_str(), missing.c_str());
            continue;
        }
        std::unique_ptr<vgd::Device> device;
        bool ok = SUCCEEDED(instance->create_device(i, &device)) && body(*device);
        std::fprintf(stderr, "%s [%s]: %s\n", test_name, a.name.c_str(), ok ? "PASS" : "FAIL");
        ran++;
        failed += !ok;
    }
    if (!ran) {
        std::fprintf(stderr, "%s: SKIP (no usable adapter)\n", test_name);
        return 77;
    }
    return failed ? 1 : 0;
}

} // namespace vgd_test
