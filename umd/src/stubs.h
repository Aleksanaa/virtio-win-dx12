// SPDX-License-Identifier: MIT
// Logging stubs for DDI slots we have not implemented. Each stub logs its table and slot name (the first few
// calls only) and returns the "not done" value for its return type: E_NOTIMPL, zero, or nothing.
#pragma once

#include "log.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace vgd::umd {

template <size_t N>
struct Name {
    char s[N];
    constexpr Name(const char (&a)[N]) { std::copy_n(a, N, s); }
};

template <Name Table, Name Slot, class F>
struct Stub;

template <Name Table, Name Slot, class R, class... A>
struct Stub<Table, Slot, R(APIENTRY*)(A...)> {
    static R APIENTRY call(A...)
    {
        static std::atomic<unsigned> calls{0};
        const unsigned n = calls.fetch_add(1, std::memory_order_relaxed);
        if (n < 3)
            VGD_WARN("UNIMPLEMENTED %s.%s%s", Table.s, Slot.s, n == 2 ? " (further calls not logged)" : "");
        if constexpr (std::is_void_v<R>)
            return;
        else if constexpr (std::is_same_v<R, HRESULT>)
            return E_NOTIMPL;
        else
            return R{};
    }
};

// Assigns a stub to a table member if the member is a function pointer (reserved void* members stay null).
template <Name Table, Name Slot, class F>
void set_stub(F& slot)
{
    if constexpr (std::is_pointer_v<F> && std::is_function_v<std::remove_pointer_t<F>>)
        slot = &Stub<Table, Slot, F>::call;
    else
        slot = nullptr;
}

} // namespace vgd::umd

// Fills every slot of `table` (an X-macro list from ddi_slots.h) with stubs tagged `name`.
#define VGD_FILL_STUBS(table, name, SLOTS)                                                                   \
    do {                                                                                                     \
        auto& vgd_t_ = (table);                                                                              \
        static constexpr ::vgd::umd::Name vgd_n_{name};                                                      \
        SLOTS(VGD_STUB_ONE_)                                                                                 \
    } while (0)
#define VGD_STUB_ONE_(m) ::vgd::umd::set_stub<vgd_n_, #m>(vgd_t_.m);
