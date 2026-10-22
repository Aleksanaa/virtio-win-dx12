// SPDX-License-Identifier: MIT
#include "vulkan_icd.h"
#include "log.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace vgd::umd {

namespace {

constexpr const wchar_t* kBundledIcd = L"vgd_vulkan_virtio.dll";

std::string read_file(const std::wstring& path)
{
    std::string data;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f)
        return data;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        data.append(buf, n);
    std::fclose(f);
    return data;
}

// The manifest's "library_path" string. Enough of JSON for ICD manifests: no escapes beyond \\ and \/.
std::string library_path(const std::string& json)
{
    const size_t key = json.find("\"library_path\"");
    if (key == std::string::npos)
        return {};
    const size_t open = json.find('"', json.find(':', key) + 1);
    if (open == std::string::npos)
        return {};
    std::string out;
    for (size_t i = open + 1; i < json.size() && json[i] != '"'; i++) {
        if (json[i] == '\\' && i + 1 < json.size())
            i++;
        out += json[i];
    }
    return out;
}

std::vector<std::wstring> registered_manifests()
{
    std::vector<std::wstring> out;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\Drivers", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; i++) {
        wchar_t name[MAX_PATH * 2];
        DWORD name_len = ARRAYSIZE(name), type = 0, value = 1, value_len = sizeof(value);
        LONG r = RegEnumValueW(key, i, name, &name_len, nullptr, &type, reinterpret_cast<BYTE*>(&value), &value_len);
        if (r == ERROR_NO_MORE_ITEMS)
            break;
        if (r == ERROR_SUCCESS && type == REG_DWORD && value == 0)   // 0 means enabled
            out.emplace_back(name, name_len);
    }
    RegCloseKey(key);
    return out;
}

std::wstring widen(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), static_cast<int>(w.size()));
    if (!w.empty())
        w.pop_back();
    return w;
}

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

// Our build of yttrium's Venus ICD (icd/), installed next to this DLL: it can export memory, which present needs.
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
    const std::wstring bundled = bundled_icd_path();
    if (!bundled.empty() && GetFileAttributesW(bundled.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (PFN_vkGetInstanceProcAddr gipa = try_load(bundled))
            return gipa;
    } else {
        VGD_WARN("icd: %ls not found, using the registered driver (no memory export: present will not work)",
                 bundled.c_str());
    }
    for (const std::wstring& manifest : registered_manifests()) {
        if (manifest.find(L"virtio") == std::wstring::npos)
            continue;
        const std::string lib = library_path(read_file(manifest));
        if (lib.empty()) {
            VGD_WARN("icd: %ls has no library_path", manifest.c_str());
            continue;
        }
        // A relative library_path is relative to the manifest's directory.
        std::wstring path = widen(lib);
        if (path.find(L'\\') == std::wstring::npos && path.find(L'/') == std::wstring::npos)
            path = manifest.substr(0, manifest.find_last_of(L"\\/") + 1) + path;
        if (PFN_vkGetInstanceProcAddr gipa = try_load(path))
            return gipa;
    }
    VGD_WARN("icd: no enabled virtio Vulkan driver registered");
    return nullptr;
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
