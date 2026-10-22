// SPDX-License-Identifier: MIT
#pragma once

#include <initializer_list>

namespace vgd::umd {

enum class LogLevel { error, warn, info, debug };

// Lines go to OutputDebugString, and also to the file named by VGD_LOG, from the environment or else from
// HKLM\SOFTWARE\vgd3d12 (for processes that do not inherit the environment, such as DWM and browsers' GPU
// processes). Each line names `component`, the UMD writing it.
void log_init(const char* component);
// Copies each named setting from HKLM\SOFTWARE\vgd3d12 into the process environment unless the environment has it:
// the engines (vkd3d-proton, DXVK) and the Venus ICD read their options with GetEnvironmentVariable.
void forward_settings(std::initializer_list<const char*> names);
// The setting `name` from the environment or else the registry (as above); false if neither has it. A REG_DWORD
// value reads as its decimal digits.
bool setting(const char* name, char* out, unsigned long size);
// VGD_VERBOSE=1 (likewise): also log every answer of high-volume queries (format support, MSAA levels).
bool verbose();
// Logs every thread's stack in the process (module+offset frames, thread names), for hangs: see stacks.cpp.
void log_thread_stacks(const char* why);
// Logs first-chance C++ throws and access violations with the raising thread's stack (stacks.cpp); log_init
// installs it when a log file is configured.
void log_exceptions();
// For DllMain when the DLL is unloaded (not at process exit): removes the exception logger, which would otherwise
// stay registered pointing into unmapped code, and closes the log file.
void log_shutdown();
void logf(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

} // namespace vgd::umd

#define VGD_TRACE(...) ::vgd::umd::logf(::vgd::umd::LogLevel::debug, __VA_ARGS__)
#define VGD_VERBOSE(...) (::vgd::umd::verbose() ? ::vgd::umd::logf(::vgd::umd::LogLevel::debug, __VA_ARGS__) : void())
#define VGD_WARN(...) ::vgd::umd::logf(::vgd::umd::LogLevel::warn, __VA_ARGS__)
