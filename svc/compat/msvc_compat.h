// SPDX-License-Identifier: MIT
// What the vendored service sources take from MSVC and GCC lacks; included first in every file (-include).
#pragma once
#ifdef __cplusplus
#include <cstring>
#include <string>

// __FUNCTIONW__ is MSVC's wide __FUNCTION__ literal. GCC has none, so widen __func__ (ASCII) at run time.
inline const wchar_t *vgd_widen_function(const char *name)
{
    thread_local std::wstring wide;
    wide.assign(name, name + std::strlen(name));
    return wide.c_str();
}
#define __FUNCTIONW__ vgd_widen_function(__func__)
#endif

// MSVC's old spelling of __cdecl, and the old SAL annotations the entry points carry. libstdc++ uses __in as a name,
// so the standard headers the sources include come first, and their include guards keep them from seeing the macro.
#ifdef __cplusplus
#include <algorithm>
#include <iostream>
#include <list>
#endif
#ifndef _cdecl
#define _cdecl __cdecl
#endif
#ifndef __in
#define __in
#endif
#ifndef __in_ecount
#define __in_ecount(n)
#endif
