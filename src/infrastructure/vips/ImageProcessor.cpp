#include "infrastructure/vips/ImageProcessor.hpp"
#include "models/HttpException.hpp"
#include <trantor/net/EventLoop.h>
#include <stdexcept>
#include <string>

static void vips_worker_thread_exit() { vips_thread_shutdown(); }

// --- resolution guard -------------------------------------------------------

static void check_src_resolution(VipsImage* img, int max_mp) {
    if (max_mp <= 0) return;
    const long px = (long)vips_image_get_width(img) * vips_image_get_height(img);
    if (px > (long)max_mp * 1'000'000) {
        vips_error_clear();
        throw HttpException(drogon::k413RequestEntityTooLarge,
            "source image resolution exceeds limit");
    }
}

// --- vips helpers -----------------------------------------------------------

static VipsSize map_resize_size(FitMode fit) {
    switch (fit) {
    case FitMode::FillDown: return VIPS_SIZE_DOWN;
    case FitMode::Force:    return VIPS_SIZE_FORCE;
    default:                return VIPS_SIZE_BOTH;
    }
}

static VipsInteresting map_resize_crop(FitMode fit) {
    if (fit == FitMode::Fill || fit == FitMode::FillDown)
        return VIPS_INTERESTING_ATTENTION;
    return VIPS_INTERESTING_NONE;
}

static VipsInteresting map_gravity(Gravity g) {
    switch (g) {
    case Gravity::Entropy:   return VIPS_INTERESTING_ENTROPY;
    case Gravity::Center:    return VIPS_INTERESTING_CENTRE;
    default:                 return VIPS_INTERESTING_ATTENTION;
    }
}

// --- save -------------------------------------------------------------------

std::string ImageProcessor::save_image(VipsImage* img, OutputFormat fmt, int quality) {
    void*  buf  = nullptr;
    size_t size = 0;
    int    r    = 0;

    // Per-format defaults match imgproxy defaults (jpeg=80, webp=75, avif=65).
    // quality -1 means "use default".
    switch (fmt) {
    case OutputFormat::WebP: {
        int q = quality > 0 ? quality : 75;
        r = vips_webpsave_buffer(img, &buf, &size,
                "Q", q, "effort", 4,
                "preset", VIPS_FOREIGN_WEBP_PRESET_DEFAULT,
                "strip", TRUE, NULL);
        break;
    }
    case OutputFormat::JPEG: {
        int q = quality > 0 ? quality : 80;
        r = vips_jpegsave_buffer(img, &buf, &size,
                "Q", q, "optimize_coding", TRUE, "strip", TRUE, NULL);
        break;
    }
    case OutputFormat::PNG:
        r = vips_pngsave_buffer(img, &buf, &size,
                "filter", VIPS_FOREIGN_PNG_FILTER_ALL, "strip", TRUE, NULL);
        break;
    case OutputFormat::AVIF: {
        int q = quality > 0 ? quality : 65;
        r = vips_heifsave_buffer(img, &buf, &size,
                "compression", VIPS_FOREIGN_HEIF_COMPRESSION_AV1,
                "Q", q, "effort", 5, "strip", TRUE, NULL);
        break;
    }
    }

    if (r != 0 || !buf) {
        std::string msg = vips_error_buffer();
        vips_error_clear();
        throw std::runtime_error("vips save failed: " + msg);
    }

    std::string result(static_cast<char*>(buf), size);
    g_free(buf);
    return result;
}

// --- sync operations --------------------------------------------------------

std::string ImageProcessor::resize_sync(
    const std::vector<uint8_t>& buf, int w, int h, FitMode fit, OutputFormat fmt, int quality,
    int max_src_resolution_mp)
{
    if (max_src_resolution_mp > 0) {
        VipsImage* hdr = vips_image_new_from_buffer(
            buf.data(), buf.size(), "[access=sequential]", NULL);
        if (!hdr) {
            std::string msg = vips_error_buffer(); vips_error_clear();
            throw std::runtime_error("vips load failed: " + msg);
        }
        VipsImageGuard g{hdr};
        check_src_resolution(hdr, max_src_resolution_mp);
    }

    const int target_w = w > 0 ? w : VIPS_MAX_COORD;
    const int target_h = h > 0 ? h : VIPS_MAX_COORD;

    VipsImage* out = nullptr;
    int r = vips_thumbnail_buffer(
        const_cast<void*>(static_cast<const void*>(buf.data())),
        buf.size(), &out, target_w,
        "height", target_h,
        "size",   map_resize_size(fit),
        "crop",   map_resize_crop(fit),
        NULL);

    if (r != 0 || !out) {
        std::string msg = vips_error_buffer();
        vips_error_clear();
        throw std::runtime_error("vips resize failed: " + msg);
    }

    VipsImageGuard img{out};
    return save_image(img.get(), fmt, quality);
}

std::string ImageProcessor::crop_sync(
    const std::vector<uint8_t>& buf, int w, int h, Gravity gravity, OutputFormat fmt, int quality,
    int max_src_resolution_mp)
{
    if (max_src_resolution_mp > 0) {
        VipsImage* hdr = vips_image_new_from_buffer(
            buf.data(), buf.size(), "[access=sequential]", NULL);
        if (!hdr) {
            std::string msg = vips_error_buffer(); vips_error_clear();
            throw std::runtime_error("vips load failed: " + msg);
        }
        VipsImageGuard g{hdr};
        check_src_resolution(hdr, max_src_resolution_mp);
    }

    VipsImage* out = nullptr;
    int r = vips_thumbnail_buffer(
        const_cast<void*>(static_cast<const void*>(buf.data())),
        buf.size(), &out, w,
        "height", h,
        "size",   VIPS_SIZE_BOTH,
        "crop",   map_gravity(gravity),
        NULL);

    if (r != 0 || !out) {
        std::string msg = vips_error_buffer();
        vips_error_clear();
        throw std::runtime_error("vips crop failed: " + msg);
    }

    VipsImageGuard img{out};
    return save_image(img.get(), fmt, quality);
}

std::string ImageProcessor::convert_sync(
    const std::vector<uint8_t>& buf, OutputFormat fmt, int quality,
    int max_src_resolution_mp)
{
    VipsImage* raw = vips_image_new_from_buffer(
        buf.data(), buf.size(), "[access=sequential]", NULL);
    if (!raw) {
        std::string msg = vips_error_buffer();
        vips_error_clear();
        throw std::runtime_error("vips load failed: " + msg);
    }

    VipsImageGuard img{raw};
    check_src_resolution(img.get(), max_src_resolution_mp);
    return save_image(img.get(), fmt, quality);
}

// --- thread pool ------------------------------------------------------------

ImageProcessor::ImageProcessor(int workers, int max_src_resolution_mp, int max_queue_size)
    : max_src_resolution_mp_(max_src_resolution_mp), max_queue_size_(max_queue_size)
{
    threads_.reserve(static_cast<size_t>(workers));
    for (int i = 0; i < workers; ++i)
        threads_.emplace_back([this] { worker_loop(); });
}

ImageProcessor::~ImageProcessor() {
    {
        std::unique_lock lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void ImageProcessor::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) break;
            task = std::move(queue_.front());
            queue_.pop();
        }
        task();
    }
    vips_worker_thread_exit();
}

// --- coroutine dispatch -----------------------------------------------------

drogon::Task<std::string> ImageProcessor::submit(
    std::function<std::string()> job)
{
    auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();

    // Heap-allocate the mutable state shared between the awaiter and the
    // worker closure.  GCC 12's coroutine optimizer can bitwise-relocate the
    // coroutine frame across a suspension point; any non-trivially-relocatable
    // object (std::function, std::vector) stored inline in the Awaiter would
    // have a dangling self-pointer after relocation.  Keeping all mutable
    // state on the heap and capturing it via shared_ptr (two plain pointers,
    // trivially relocatable) avoids the hazard entirely.
    struct State {
        std::function<std::string()> job;
        std::string                  result;
        std::exception_ptr           exc;
    };
    auto state = std::make_shared<State>(State{std::move(job), {}, nullptr});

    struct Awaiter {
        ImageProcessor*          self;
        std::shared_ptr<State>   state;
        trantor::EventLoop*      loop;

        bool await_ready() { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            std::unique_lock lock(self->mu_);
            if (self->max_queue_size_ > 0 &&
                (int)self->queue_.size() >= self->max_queue_size_)
            {
                lock.unlock();
                throw HttpException(drogon::k429TooManyRequests, "too many requests");
            }
            self->queue_.push([state = state, loop = loop, h]() mutable {
                try   { state->result = state->job(); }
                catch (...) { state->exc = std::current_exception(); }
                if (loop) loop->queueInLoop([h]() mutable { h.resume(); });
                else       h.resume();
            });
            lock.unlock();
            self->cv_.notify_one();
        }

        std::string await_resume() {
            if (state->exc) std::rethrow_exception(state->exc);
            return std::move(state->result);
        }
    };

    co_return co_await Awaiter{this, state, loop};
}

drogon::Task<std::string> ImageProcessor::resize(
    std::vector<uint8_t> buf, int w, int h, FitMode fit, OutputFormat fmt, int quality)
{
    auto sbuf = std::make_shared<std::vector<uint8_t>>(std::move(buf));
    const int max_mp = max_src_resolution_mp_;
    co_return co_await submit([sbuf, w, h, fit, fmt, quality, max_mp] {
        return resize_sync(*sbuf, w, h, fit, fmt, quality, max_mp);
    });
}

drogon::Task<std::string> ImageProcessor::crop(
    std::vector<uint8_t> buf, int w, int h, Gravity gravity, OutputFormat fmt, int quality)
{
    auto sbuf = std::make_shared<std::vector<uint8_t>>(std::move(buf));
    const int max_mp = max_src_resolution_mp_;
    co_return co_await submit([sbuf, w, h, gravity, fmt, quality, max_mp] {
        return crop_sync(*sbuf, w, h, gravity, fmt, quality, max_mp);
    });
}

drogon::Task<std::string> ImageProcessor::convert(
    std::vector<uint8_t> buf, OutputFormat fmt, int quality)
{
    auto sbuf = std::make_shared<std::vector<uint8_t>>(std::move(buf));
    const int max_mp = max_src_resolution_mp_;
    co_return co_await submit([sbuf, fmt, quality, max_mp] {
        return convert_sync(*sbuf, fmt, quality, max_mp);
    });
}
