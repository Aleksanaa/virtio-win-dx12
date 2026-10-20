// SPDX-License-Identifier: MIT
#pragma once

#include <vgd/core.h>

namespace vgd::umd {

// Lines go to OutputDebugString, and also to the file named by the VGD_LOG environment variable if set
// (the harness sets it machine-wide so the runtime's processes inherit it).
void log_init();
void logf(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

} // namespace vgd::umd

#define VGD_TRACE(...) ::vgd::umd::logf(::vgd::LogLevel::debug, __VA_ARGS__)
#define VGD_WARN(...) ::vgd::umd::logf(::vgd::LogLevel::warn, __VA_ARGS__)
