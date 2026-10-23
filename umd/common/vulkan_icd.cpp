// SPDX-License-Identifier: MIT
#include "vulkan_icd.h"
#include "log.h"

#include <windows.h>

#include <mutex>
#include <string>

namespace vgd::umd {

namespace {

constexpr const wchar_t* kBundledIcd = L"vgd_vulkan_virtio.dll";

PFN_vkGetInstanceProcAddr try_load(const std::wstring& path)
{
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        VGD_WARN("icd: LoadLibrary(%ls) failed: %lu", path.c_str(), GetLastError());
        return nullptr;
    }
    using Negotiate = VkResult(VKAPI_PTR*)(uint32_t*);
    auto negotiate = reinterpret_cast<Negotiate>(
        reinterpret_cast<void*>(GetProcAddress(mod, "vk_icdNegotiateLoaderICDInterfaceVersion")));
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(mod, "vk_icdGetInstanceProcAddr")));
    if (!gipa) {
        VGD_WARN("icd: %ls exports no vk_icdGetInstanceProcAddr", path.c_str());
        FreeLibrary(mod);
        return nullptr;
    }
    uint32_t version = 5;
    if (negotiate)
        negotiate(&version);
    // Size and time identify which build is installed.
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    SYSTEMTIME t{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs))
        FileTimeToSystemTime(&attrs.ftLastWriteTime, &t);
    VGD_TRACE("icd: loaded %ls (loader interface %u, %lu bytes, written %04u-%02u-%02u %02u:%02u:%02u UTC)",
              path.c_str(), version, attrs.nFileSizeLow, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return gipa;
}

// Our build of yttrium's Venus ICD (icd/), which the driver package installs next to this DLL.
std::wstring bundled_icd_path()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&bundled_icd_path), &self))
        return {};
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(self, buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf))
        return {};
    std::wstring path(buf, n);
    return path.substr(0, path.find_last_of(L"\\/") + 1) + kBundledIcd;
}

PFN_vkGetInstanceProcAddr load()
{
    const std::wstring path = bundled_icd_path();
    if (path.empty()) {
        VGD_WARN("icd: cannot locate this DLL");
        return nullptr;
    }
    return try_load(path);
}

} // namespace

PFN_vkGetInstanceProcAddr load_venus_icd()
{
    static std::once_flag once;
    static PFN_vkGetInstanceProcAddr gipa = nullptr;
    std::call_once(once, [] { gipa = load(); });
    return gipa;
}

} // namespace vgd::umd
