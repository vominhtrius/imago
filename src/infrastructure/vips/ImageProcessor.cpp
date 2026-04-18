#include "infrastructure/vips/ImageProcessor.hpp"
#include "models/HttpException.hpp"
#include <trantor/net/EventLoop.h>
#include <algorithm>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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

// --- shrink-on-load planner (imgproxy parity) -------------------------------
// Header probe → compute shrink factor → reload with the loader-specific
// shrink option so libjpeg/libwebp output at reduced resolution directly.
// Avoids the full-pixel decode that vips_thumbnail_image would do.

static std::string_view detect_loader_kind(const void* data, size_t size) {
    const char* name = vips_foreign_find_load_buffer(data, size);
    if (!name) return {};
    std::string_view sv{name};
    if (sv.find("Jpeg") != std::string_view::npos) return "jpeg";
    if (sv.find("Webp") != std::string_view::npos) return "webp";
    if (sv.find("Heif") != std::string_view::npos) return "heif";
    return {};
}

// Returns the shrink-on-load options string (e.g. "[access=sequential,shrink=4]")
// when shrink would help, std::nullopt when no shrink is beneficial — in that
// case the caller can reuse the header-probe image directly (single-load fast
// path).
static std::optional<std::string> shrink_load_options(
    std::string_view loader, int src_w, int src_h, int target_w, int target_h)
{
    double scale = -1.0;
    if (target_w > 0 && target_h > 0 && src_w > 0 && src_h > 0) {
        scale = std::max(static_cast<double>(target_w) / src_w,
                         static_cast<double>(target_h) / src_h);
    } else if (target_w > 0 && src_w > 0) {
        scale = static_cast<double>(target_w) / src_w;
    } else if (target_h > 0 && src_h > 0) {
        scale = static_cast<double>(target_h) / src_h;
    }
    if (scale <= 0.0 || scale >= 1.0) return std::nullopt;

    if (loader == "jpeg") {
        int shrink = 1;
        if      (scale <= 0.125) shrink = 8;
        else if (scale <= 0.25)  shrink = 4;
        else if (scale <= 0.5)   shrink = 2;
        if (shrink == 1) return std::nullopt;
        char out[48];
        std::snprintf(out, sizeof(out), "[access=sequential,shrink=%d]", shrink);
        return std::string{out};
    }
    if (loader == "webp") {
        // Discrete steps avoid locale-sensitive float formatting.
        if (scale <= 0.125) return std::string{"[access=sequential,scale=0.125]"};
        if (scale <= 0.25)  return std::string{"[access=sequential,scale=0.25]"};
        if (scale <= 0.5)   return std::string{"[access=sequential,scale=0.5]"};
        return std::nullopt;
    }
    // PNG / HEIF / AVIF: no usable shrink-on-load via this path.
    return std::nullopt;
}

// Header probe + (optional) shrink-aware reload.
//   * Always probes the header so we can run the resolution security check
//     and compute the optimal shrink factor.
//   * Reloads with the loader-specific shrink option only when shrink would
//     actually reduce the decode cost. Otherwise returns the probe image
//     directly (single buffer open).
static VipsImageGuard load_with_shrink_for_target(
    const std::vector<uint8_t>& buf, int max_src_resolution_mp,
    int target_w, int target_h)
{
    VipsImage* hdr = vips_image_new_from_buffer(
        buf.data(), buf.size(), "[access=sequential]", NULL);
    if (!hdr) {
        std::string msg = vips_error_buffer(); vips_error_clear();
        throw std::runtime_error("vips load failed: " + msg);
    }
    VipsImageGuard probe{hdr};
    check_src_resolution(hdr, max_src_resolution_mp);

    auto opts = shrink_load_options(
        detect_loader_kind(buf.data(), buf.size()),
        vips_image_get_width(hdr), vips_image_get_height(hdr),
        target_w, target_h);

    if (!opts) {
        // No shrink-on-load benefit — reuse the lazy probe image as the
        // pipeline source. One buffer open total.
        return probe;
    }

    VipsImage* raw_src = vips_image_new_from_buffer(
        buf.data(), buf.size(), opts->c_str(), NULL);
    if (!raw_src) {
        std::string msg = vips_error_buffer(); vips_error_clear();
        throw std::runtime_error("vips reload failed: " + msg);
    }
    return VipsImageGuard{raw_src};
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
// Encoders write directly into the destination std::string via a custom
// VipsTarget callback. Removes the intermediate g_malloc'd buffer that
// vips_xxxsave_buffer would otherwise allocate (and that we'd memcpy out of).
// Peak output-side memory drops from 2× encoded size to 1×.

extern "C" {
static gint64 vips_string_target_write(VipsTargetCustom* /*target*/,
                                       const void* data, gint64 length,
                                       gpointer user_data)
{
    auto* sink = static_cast<std::string*>(user_data);
    try {
        sink->append(static_cast<const char*>(data),
                     static_cast<std::size_t>(length));
        return length;
    } catch (...) {
        // bad_alloc → tell vips the write failed; vips_xxxsave_target will
        // return non-zero and the caller throws.
        return -1;
    }
}
} // extern "C"

std::string ImageProcessor::save_image(VipsImage* img, OutputFormat fmt, int quality) {
    std::string sink;
    sink.reserve(64 * 1024); // typical thumbnail upper bound; grows if needed

    VipsTargetCustom* target = vips_target_custom_new();
    if (!target) {
        std::string msg = vips_error_buffer(); vips_error_clear();
        throw std::runtime_error("vips target alloc failed: " + msg);
    }
    // Owns the only ref to `target`; freed unconditionally below.
    g_signal_connect(target, "write",
                     G_CALLBACK(vips_string_target_write), &sink);

    int r = 0;

    // Per-format defaults match imgproxy (jpeg=80, webp=75, avif=65).
    // quality -1 means "use default".
    switch (fmt) {
    case OutputFormat::WebP: {
        int q = quality > 0 ? quality : 75;
        r = vips_webpsave_target(img, VIPS_TARGET(target),
                "Q", q, "effort", 4,
                "preset", VIPS_FOREIGN_WEBP_PRESET_DEFAULT,
                "strip", TRUE, NULL);
        break;
    }
    case OutputFormat::JPEG: {
        int q = quality > 0 ? quality : 80;
        r = vips_jpegsave_target(img, VIPS_TARGET(target),
                "Q", q, "optimize_coding", TRUE, "strip", TRUE, NULL);
        break;
    }
    case OutputFormat::PNG:
        r = vips_pngsave_target(img, VIPS_TARGET(target),
                "filter", VIPS_FOREIGN_PNG_FILTER_ALL, "strip", TRUE, NULL);
        break;
    case OutputFormat::AVIF: {
        int q = quality > 0 ? quality : 65;
        r = vips_heifsave_target(img, VIPS_TARGET(target),
                "compression", VIPS_FOREIGN_HEIF_COMPRESSION_AV1,
                "Q", q, "effort", 5, "strip", TRUE, NULL);
        break;
    }
    }

    g_object_unref(target);

    if (r != 0) {
        std::string msg = vips_error_buffer();
        vips_error_clear();
        throw std::runtime_error("vips save failed: " + msg);
    }

    return sink;
}

// --- sync operations --------------------------------------------------------

std::string ImageProcessor::resize_sync(
    const std::vector<uint8_t>& buf, int w, int h, FitMode fit, OutputFormat fmt, int quality,
    int max_src_resolution_mp)
{
    VipsImageGuard src = load_with_shrink_for_target(
        buf, max_src_resolution_mp, w, h);

    const int target_w = w > 0 ? w : VIPS_MAX_COORD;
    const int target_h = h > 0 ? h : VIPS_MAX_COORD;

    // fill / fill-down / force all need both dims to bound the target.
    // With only one provided, the other is VIPS_MAX_COORD and the scaler
    // blows the canvas up to ~10M px on that axis (OOM for fill; encoder
    // overflow for force). Collapse to proportional bounding-box scaling.
    const bool one_dim = (w <= 0 || h <= 0);
    VipsSize        size = one_dim ? VIPS_SIZE_BOTH          : map_resize_size(fit);
    VipsInteresting crop = one_dim ? VIPS_INTERESTING_NONE   : map_resize_crop(fit);

    VipsImage* raw_out = nullptr;
    int r = vips_thumbnail_image(src.get(), &raw_out, target_w,
        "height", target_h,
        "size",   size,
        "crop",   crop,
        NULL);

    if (r != 0 || !raw_out) {
        std::string msg = vips_error_buffer();
        vips_error_clear();
        throw std::runtime_error("vips resize failed: " + msg);
    }

    VipsImageGuard out{raw_out};
    return save_image(out.get(), fmt, quality);
}

std::string ImageProcessor::crop_sync(
    const std::vector<uint8_t>& buf, int w, int h, Gravity gravity, OutputFormat fmt, int quality,
    int max_src_resolution_mp)
{
    VipsImageGuard src = load_with_shrink_for_target(
        buf, max_src_resolution_mp, w, h);

    VipsImage* raw_out = nullptr;
    int r = vips_thumbnail_image(src.get(), &raw_out, w,
        "height", h,
        "size",   VIPS_SIZE_BOTH,
        "crop",   map_gravity(gravity),
        NULL);

    if (r != 0 || !raw_out) {
        std::string msg = vips_error_buffer();
        vips_error_clear();
        throw std::runtime_error("vips crop failed: " + msg);
    }

    VipsImageGuard out{raw_out};
    return save_image(out.get(), fmt, quality);
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
    std::vector<uint8_t> buf,
    std::function<std::string(const std::vector<uint8_t>&)> job)
{
    auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();

    // Heap-allocate the mutable state shared between the awaiter and the
    // worker closure. The buffer lives *inside* State (no separate
    // shared_ptr<vector> wrapper), saving one allocation + two atomic
    // ref-count ops per request. Two refs total: the awaiter and the queued
    // task.
    //
    // GCC 12's coroutine optimizer can bitwise-relocate the coroutine frame
    // across a suspension point; non-trivially-relocatable objects (vector,
    // function) stored inline in the Awaiter would have dangling self-pointers
    // after relocation. Keeping them on the heap behind a shared_ptr (two
    // plain pointers, trivially relocatable) avoids the hazard.
    struct State {
        std::vector<uint8_t>                                       buf;
        std::function<std::string(const std::vector<uint8_t>&)>    job;
        std::string                                                result;
        std::exception_ptr                                         exc;
    };
    auto state = std::make_shared<State>();
    state->buf = std::move(buf);
    state->job = std::move(job);

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
                try   { state->result = state->job(state->buf); }
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
    const int max_mp = max_src_resolution_mp_;
    co_return co_await submit(std::move(buf),
        [w, h, fit, fmt, quality, max_mp](const std::vector<uint8_t>& b) {
            return resize_sync(b, w, h, fit, fmt, quality, max_mp);
        });
}

drogon::Task<std::string> ImageProcessor::crop(
    std::vector<uint8_t> buf, int w, int h, Gravity gravity, OutputFormat fmt, int quality)
{
    const int max_mp = max_src_resolution_mp_;
    co_return co_await submit(std::move(buf),
        [w, h, gravity, fmt, quality, max_mp](const std::vector<uint8_t>& b) {
            return crop_sync(b, w, h, gravity, fmt, quality, max_mp);
        });
}

drogon::Task<std::string> ImageProcessor::convert(
    std::vector<uint8_t> buf, OutputFormat fmt, int quality)
{
    const int max_mp = max_src_resolution_mp_;
    co_return co_await submit(std::move(buf),
        [fmt, quality, max_mp](const std::vector<uint8_t>& b) {
            return convert_sync(b, fmt, quality, max_mp);
        });
}
