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

void sink(LogLevel level, const char* msg) { logf(level, "core: %s", msg); }

// The environment variable, or else the same-named value under HKLM\SOFTWARE\vgd3d12: processes that do not
// inherit the environment (Chromium's GPU process gets a filtered one) still read the registry.
bool setting(const char* name, char* out, DWORD size)
{
    const DWORD len = GetEnvironmentVariableA(name, out, size);
    if (len > 0 && len < size)
        return true;
    DWORD bytes = size;
    return RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\vgd3d12", name, RRF_RT_REG_SZ, nullptr, out, &bytes) ==
               ERROR_SUCCESS &&
           out[0];
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
    set_log_sink(sink);
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

    char line[1100];
    std::snprintf(line, sizeof(line), "vgd3d12[%lu:%lu] %s %s\n", GetCurrentProcessId(), GetCurrentThreadId(),
                  names[static_cast<int>(level)], body);
    OutputDebugStringA(line);
    std::lock_guard lock(g_lock);
    if (g_file) {
        std::fputs(line, g_file);
        std::fflush(g_file);
    }
}

} // namespace vgd::umd
