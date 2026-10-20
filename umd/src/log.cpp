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

} // namespace

void log_init()
{
    std::lock_guard lock(g_lock);
    if (g_file)
        return;
    char path[MAX_PATH];
    const DWORD len = GetEnvironmentVariableA("VGD_LOG", path, sizeof(path));
    if (len > 0 && len < sizeof(path))
        g_file = std::fopen(path, "a");
    char v[8];
    g_verbose = GetEnvironmentVariableA("VGD_VERBOSE", v, sizeof(v)) > 0 && v[0] == '1';
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
