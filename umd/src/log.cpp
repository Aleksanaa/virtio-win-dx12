// SPDX-License-Identifier: MIT
#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace vgd::umd {

namespace {

std::mutex g_lock;
FILE* g_file = nullptr;
bool g_verbose = false;

// The environment variable, or else the same-named value under HKLM\SOFTWARE\vgd3d12: processes that do not
// inherit the environment (Chromium's GPU process gets a filtered one) still read the registry. A REG_DWORD value
// reads as its decimal digits.
bool setting(const char* name, char* out, DWORD size)
{
    const DWORD len = GetEnvironmentVariableA(name, out, size);
    if (len > 0 && len < size)
        return true;
    DWORD bytes = size;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\vgd3d12", name, RRF_RT_REG_SZ, nullptr, out, &bytes) ==
        ERROR_SUCCESS)
        return out[0] != 0;
    DWORD value = 0;
    bytes = sizeof(value);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\vgd3d12", name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) !=
        ERROR_SUCCESS)
        return false;
    std::snprintf(out, size, "%lu", value);
    return true;
}

} // namespace

void log_init()
{
    std::lock_guard lock(g_lock);
    if (g_file)
        return;
    char path[MAX_PATH];
    if (setting("VGD_LOG", path, sizeof(path)))
        g_file = std::fopen(path, "a");
    char v[8];
    g_verbose = setting("VGD_VERBOSE", v, sizeof(v)) && v[0] == '1';
    // vkd3d-proton and the Venus ICD read their options with GetEnvironmentVariable: give them the registry's too.
    for (const char* name : {"VKD3D_DEBUG", "VKD3D_SHADER_DEBUG", "VKD3D_LOG_FILE", "VKD3D_CONFIG", "VKD3D_FEATURE_LEVEL",
                             "VN_DEBUG", "VN_PERF", "MESA_LOG_LEVEL"}) {
        char value[256];
        if (setting(name, value, sizeof(value)))
            SetEnvironmentVariableA(name, value);
    }
}

bool verbose() { return g_verbose; }

void logf(LogLevel level, const char* fmt, ...)
{
    static const char* names[] = {"E", "W", "I", "D"};
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    // Local wall-clock time, to line up with logs from the host.
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1120];
    std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u vgd3d12[%lu:%lu] %s %s\n", t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), names[static_cast<int>(level)], body);
    OutputDebugStringA(line);
    std::lock_guard lock(g_lock);
    if (g_file) {
        std::fputs(line, g_file);
        std::fflush(g_file);
    }
}

} // namespace vgd::umd
