// SPDX-License-Identifier: MIT
// A reference-counted COM pointer (MinGW has no WRL).
#pragma once

#include <unknwn.h>

#include <utility>

namespace vgd::umd {

template <class T>
class Com {
public:
    Com() = default;
    Com(std::nullptr_t) {}
    // Adopts a reference the caller already holds.
    static Com adopt(T* p)
    {
        Com c;
        c.p_ = p;
        return c;
    }
    Com(const Com& o) : p_(o.p_)
    {
        if (p_)
            p_->AddRef();
    }
    Com(Com&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    Com& operator=(Com o) noexcept
    {
        std::swap(p_, o.p_);
        return *this;
    }
    ~Com()
    {
        if (p_)
            p_->Release();
    }

    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    // For out-parameters: releases what is held and returns the slot.
    T** put()
    {
        *this = nullptr;
        return &p_;
    }
    void** put_void() { return reinterpret_cast<void**>(put()); }

    // The same object as another interface; empty if it does not implement it.
    template <class U>
    Com<U> as() const
    {
        Com<U> u;
        if (p_)
            p_->QueryInterface(__uuidof(U), u.put_void());
        return u;
    }

private:
    T* p_ = nullptr;
};

} // namespace vgd::umd
