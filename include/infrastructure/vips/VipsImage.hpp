#pragma once
#include <vips/vips.h>
#include <utility>

// Move-only RAII wrapper for VipsImage*.
// Never hold a raw VipsImage* across a co_await suspension point —
// move it into the thread pool closure first.
class VipsImageGuard {
    VipsImage* img_ = nullptr;

public:
    VipsImageGuard() = default;
    explicit VipsImageGuard(VipsImage* img) : img_(img) {}

    ~VipsImageGuard() {
        if (img_) g_object_unref(img_);
    }

    VipsImageGuard(const VipsImageGuard&)            = delete;
    VipsImageGuard& operator=(const VipsImageGuard&) = delete;

    VipsImageGuard(VipsImageGuard&& o) noexcept
        : img_(std::exchange(o.img_, nullptr)) {}

    VipsImageGuard& operator=(VipsImageGuard&& o) noexcept {
        if (this != &o) {
            if (img_) g_object_unref(img_);
            img_ = std::exchange(o.img_, nullptr);
        }
        return *this;
    }

    VipsImage* get() const noexcept { return img_; }
    VipsImage* release() noexcept   { return std::exchange(img_, nullptr); }
    explicit operator bool() const noexcept { return img_ != nullptr; }
};
