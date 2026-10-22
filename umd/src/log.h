// SPDX-License-Identifier: MIT
#pragma once

namespace vgd::umd {

enum class LogLevel { error, warn, info, debug };

// Lines go to OutputDebugString, and also to the file named by VGD_LOG, from the environment or else from
// HKLM\SOFTWARE\vgd3d12 (for processes that do not inherit the environment, such as browsers' GPU processes).
// vkd3d-proton's and the Venus ICD's own options (VKD3D_*, VN_*) are forwarded the same way.
void log_init();
// VGD_VERBOSE=1 (likewise): also log every answer of high-volume queries (format support, MSAA levels).
bool verbose();
void logf(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

} // namespace vgd::umd

#define VGD_TRACE(...) ::vgd::umd::logf(::vgd::umd::LogLevel::debug, __VA_ARGS__)
#define VGD_VERBOSE(...) (::vgd::umd::verbose() ? ::vgd::umd::logf(::vgd::umd::LogLevel::debug, __VA_ARGS__) : void())
#define VGD_WARN(...) ::vgd::umd::logf(::vgd::umd::LogLevel::warn, __VA_ARGS__)
