// SPDX-License-Identifier: MIT
#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace vgd::umd {

namespace {

std::mutex g_lock;
HANDLE g_file = INVALID_HANDLE_VALUE;
bool g_verbose = false;
const char* g_component = "vgd";

constexpr const char* kKey = "SOFTWARE\\vgd3d12";

} // namespace

bool setting(const char* name, char* out, unsigned long size)
{
    const DWORD len = GetEnvironmentVariableA(name, out, size);
    if (len > 0 && len < size)
        return true;
    DWORD bytes = size;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, kKey, name, RRF_RT_REG_SZ, nullptr, out, &bytes) == ERROR_SUCCESS)
        return out[0] != 0;
    DWORD value = 0;
    bytes = sizeof(value);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS)
        return false;
    std::snprintf(out, size, "%lu", value);
    return true;
}

void forward_settings(std::initializer_list<const char*> names)
{
    for (const char* name : names) {
        char value[256];
        if (GetEnvironmentVariableA(name, nullptr, 0) == 0 && setting(name, value, sizeof(value)))
            SetEnvironmentVariableA(name, value);
    }
}

void log_init(const char* component)
{
    std::lock_guard lock(g_lock);
    g_component = component;
    if (g_file != INVALID_HANDLE_VALUE)
        return;
    char path[MAX_PATH];
    // Append-only access makes each line one atomic append, so processes sharing the file do not cut into each
    // other's lines (the CRT's append mode seeks, then writes). Write-through keeps the log when the VM is killed
    // because the desktop never came up.
    if (setting("VGD_LOG", path, sizeof(path)))
        g_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (g_file != INVALID_HANDLE_VALUE)
        log_exceptions();
    char v[8];
    g_verbose = setting("VGD_VERBOSE", v, sizeof(v)) && v[0] == '1';
}

void log_exceptions_off(); // stacks.cpp

void log_shutdown()
{
    // A handler left behind outlives the DLL: the process's next exception calls into unmapped code, which CFG
    // turns into a fast fail in ntdll (dxdiag and Task Manager close the adapter they queried and run on).
    log_exceptions_off();
    std::lock_guard lock(g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
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
    std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u %s[%lu:%lu] %s %s\n", t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds, g_component, GetCurrentProcessId(), GetCurrentThreadId(),
                  names[static_cast<int>(level)], body);
    OutputDebugStringA(line);
    std::lock_guard lock(g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    }
}

} // namespace vgd::umd
