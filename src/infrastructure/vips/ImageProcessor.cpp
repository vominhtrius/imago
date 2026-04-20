#include "infrastructure/vips/ImageProcessor.hpp"
#include "models/HttpException.hpp"
#include "utils/HttpUtils.hpp"
#include <trantor/net/EventLoop.h>
#include <algorithm>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

static void vips_worker_thread_exit() { vips_thread_shutdown(); }

// --- resolution guard -------------------------------------------------------

static void check_src_resolution(VipsImage* img, int max_mp) {
    if (max_mp <= 0) return;
    // Use 64-bit arithmetic — a 40000×40000 source (1.6 Gpx) overflows `long`
    // on 32-bit platforms. The comparison below must also be 64-bit.
    const long long px = static_cast<long long>(vips_image_get_width(img)) *
                         static_cast<long long>(vips_image_get_height(img));
    if (px > static_cast<long long>(max_mp) * 1'000'000LL) {
        // No vips error was set at this point — width/height getters don't
        // populate the error buffer. Don't clobber unrelated state.
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

// Fast-path gravities: ones libvips can satisfy in a single
// vips_thumbnail_image call via its `crop` (VipsInteresting) parameter.
// Everything else needs the cover-scale + extract_area slow path because
// VipsInteresting has no directional (N/S/E/W/corner) or focus-point modes.
static bool is_fast_path_gravity(Gravity g) {
    return g == Gravity::Smart || g == Gravity::Entropy || g == Gravity::Center;
}

static VipsInteresting map_fast_gravity(Gravity g) {
    switch (g) {
    case Gravity::Entropy: return VIPS_INTERESTING_ENTROPY;
    case Gravity::Center:  return VIPS_INTERESTING_CENTRE;
    case Gravity::Smart:   return VIPS_INTERESTING_ATTENTION;
    default:               return VIPS_INTERESTING_CENTRE;  // unreachable
    }
}

// For directional/corner/focus-point crop: return the top-left offset of a
// target_w × target_h window inside a scaled_w × scaled_h canvas. Offsets are
// clamped so the window always stays inside the canvas.
static void gravity_offset(Gravity g, double fp_x, double fp_y,
                           int scaled_w, int scaled_h,
                           int target_w, int target_h,
                           int& left, int& top)
{
    const int dx = std::max(0, scaled_w - target_w);
    const int dy = std::max(0, scaled_h - target_h);

    auto mid_x = dx / 2;
    auto mid_y = dy / 2;

    switch (g) {
    case Gravity::North:     left = mid_x; top = 0;     break;
    case Gravity::South:     left = mid_x; top = dy;    break;
    case Gravity::East:      left = dx;    top = mid_y; break;
    case Gravity::West:      left = 0;     top = mid_y; break;
    case Gravity::NorthEast: left = dx;    top = 0;     break;
    case Gravity::NorthWest: left = 0;     top = 0;     break;
    case Gravity::SouthEast: left = dx;    top = dy;    break;
    case Gravity::SouthWest: left = 0;     top = dy;    break;
    case Gravity::FocusPoint: {
        // Focus point centered in target window, clamped to canvas.
        double cx = fp_x * scaled_w;
        double cy = fp_y * scaled_h;
        long   l  = static_cast<long>(cx) - target_w / 2;
        long   t  = static_cast<long>(cy) - target_h / 2;
        if (l < 0) l = 0; else if (l > dx) l = dx;
        if (t < 0) t = 0; else if (t > dy) t = dy;
        left = static_cast<int>(l);
        top  = static_cast<int>(t);
        break;
    }
    default: left = mid_x; top = mid_y;  // Center fallback
    }
}

// --- metadata scrub (imgproxy parity) ---------------------------------------
// imgproxy's default: strip EXIF/IPTC/XMP, keep ICC, keep a whitelist of
// benign/useful EXIF fields (copyright, orientation, resolution, color space).
// libvips' `keep=VIPS_FOREIGN_KEEP_EXIF | KEEP_ICC` preserves the full EXIF
// block; we mutate the image metadata *before* save to drop the rest.
//
// EXIF fields surface as `exif-ifd{N}-{Name}` where N is the IFD number:
//   0 = main (camera, copyright, orientation)
//   1 = thumbnail (always drop — we're emitting a transcode)
//   2 = Exif subIFD (capture settings, DateTime, MakerNote — mostly drop)
//   3 = GPS (always drop — privacy)

static bool exif_suffix_whitelisted(std::string_view suffix) {
    // Whitelist matches imgproxy's default carry-through set.
    static constexpr std::string_view keep[] = {
        "Copyright", "Artist",
        "Orientation",
        "XResolution", "YResolution", "ResolutionUnit",
        "PixelXDimension", "PixelYDimension",
    };
    for (auto w : keep) if (suffix == w) return true;
    return false;
}

extern "C" {
static void* collect_strip_field(VipsImage* /*img*/, const char* name,
                                 GValue* /*value*/, void* user_data)
{
    auto* out = static_cast<std::vector<std::string>*>(user_data);
    std::string_view sv{name};

    // Drop entire blobs.
    if (sv.starts_with("xmp-") || sv == "xmp-data") {
        out->emplace_back(name); return nullptr;
    }
    if (sv.starts_with("iptc-") || sv == "iptc-data") {
        out->emplace_back(name); return nullptr;
    }

    // EXIF: always drop thumbnail (ifd1) and GPS (ifd3) IFDs.
    if (sv.starts_with("exif-ifd1-") || sv.starts_with("exif-ifd3-")) {
        out->emplace_back(name); return nullptr;
    }

    // EXIF main (ifd0) and Exif subIFD (ifd2): keep only whitelisted suffixes.
    auto check_ifd = [&](std::string_view prefix) {
        if (!sv.starts_with(prefix)) return false;
        std::string_view suffix = sv.substr(prefix.size());
        // libvips suffixes sometimes carry a trailing type tag ("-Short");
        // compare the head token only.
        auto dash = suffix.find('-');
        std::string_view head = dash == std::string_view::npos
            ? suffix : suffix.substr(0, dash);
        if (!exif_suffix_whitelisted(head)) out->emplace_back(name);
        return true;
    };
    if (check_ifd("exif-ifd0-")) return nullptr;
    if (check_ifd("exif-ifd2-")) return nullptr;

    return nullptr;
}
} // extern "C"

static void strip_sensitive_metadata(VipsImage* img) {
    std::vector<std::string> to_remove;
    vips_image_map(img, collect_strip_field, &to_remove);
    for (const auto& name : to_remove) vips_image_remove(img, name.c_str());

    // Belt-and-braces: remove the top-level XMP/IPTC blobs by their canonical
    // vips metadata names in case the loader stored them under a distinct key.
    vips_image_remove(img, VIPS_META_XMP_NAME);
    vips_image_remove(img, VIPS_META_IPTC_NAME);

    // Drop the embedded JPEG thumbnail that libvips stashes separately
    // from the `exif-data` blob. Without this the JPEG save re-emits the
    // source's IFD1 thumbnail (typically ~18 KB), dwarfing the resized
    // output for small thumbnails. imgproxy strips it the same way.
    vips_image_remove(img, "jpeg-thumbnail-data");
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

    // Selective metadata preservation — imgproxy parity. Scrub first (drop
    // XMP, IPTC, GPS, thumbnail IFD, and non-whitelisted EXIF fields) and
    // then ask the encoder to carry forward EXIF + ICC only.
    strip_sensitive_metadata(img);
    const int keep = VIPS_FOREIGN_KEEP_EXIF | VIPS_FOREIGN_KEEP_ICC;

    // Per-format defaults match imgproxy (jpeg=80, webp=75, avif=65).
    // quality -1 means "use default".
    switch (fmt) {
    case OutputFormat::Auto:
        // Use cases resolve Auto → concrete before dispatching; anything
        // reaching the encoder with Auto is a caller bug.
        g_object_unref(target);
        throw std::runtime_error(
            "save_image: OutputFormat::Auto must be resolved before save");
    case OutputFormat::WebP: {
        int q = quality > 0 ? quality : 75;
        r = vips_webpsave_target(img, VIPS_TARGET(target),
                "Q", q, "effort", 4,
                "preset", VIPS_FOREIGN_WEBP_PRESET_DEFAULT,
                "keep", keep, NULL);
        break;
    }
    case OutputFormat::JPEG: {
        int q = quality > 0 ? quality : 80;
        r = vips_jpegsave_target(img, VIPS_TARGET(target),
                "Q", q, "optimize_coding", TRUE, "keep", keep, NULL);
        break;
    }
    case OutputFormat::PNG:
        r = vips_pngsave_target(img, VIPS_TARGET(target),
                "filter", VIPS_FOREIGN_PNG_FILTER_ALL, "keep", keep, NULL);
        break;
    case OutputFormat::AVIF: {
        int q = quality > 0 ? quality : 65;
        r = vips_heifsave_target(img, VIPS_TARGET(target),
                "compression", VIPS_FOREIGN_HEIF_COMPRESSION_AV1,
                "Q", q, "effort", 5, "keep", keep, NULL);
        break;
    }
    default:
        // Defence in depth: OutputFormat is a closed enum, but a future
        // addition that forgets to extend this switch would otherwise return
        // an empty `sink` while leaking `target`. Free + throw instead.
        g_object_unref(target);
        throw std::runtime_error("save_image: unsupported OutputFormat");
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
    const std::vector<uint8_t>& buf, int w, int h, Gravity gravity,
    double fp_x, double fp_y, OutputFormat fmt, int quality,
    int max_src_resolution_mp)
{
    VipsImageGuard src = load_with_shrink_for_target(
        buf, max_src_resolution_mp, w, h);

    // Fast path: content-aware (Smart/Entropy) and plain Center can be served
    // by vips_thumbnail_image's built-in crop modes — one op, best case.
    if (is_fast_path_gravity(gravity)) {
        VipsImage* raw_out = nullptr;
        int r = vips_thumbnail_image(src.get(), &raw_out, w,
            "height", h,
            "size",   VIPS_SIZE_BOTH,
            "crop",   map_fast_gravity(gravity),
            NULL);

        if (r != 0 || !raw_out) {
            std::string msg = vips_error_buffer();
            vips_error_clear();
            throw std::runtime_error("vips crop failed: " + msg);
        }
        VipsImageGuard out{raw_out};
        return save_image(out.get(), fmt, quality);
    }

    // Slow path: cover-scale + extract_area for directional / corner / focus
    // point gravities. libvips' VipsInteresting has no per-axis option, so
    // build the window manually.
    //
    //   cover = max(target_w / src_w, target_h / src_h)
    //   resize(src, cover) → canvas that just covers the target
    //   extract_area(canvas, gravity-derived offset, target_w, target_h)
    //
    // Shrink-on-load has already brought the source close to the target size,
    // so `cover` here is typically in [0.5, 2.0].

    // Apply EXIF orientation so the window is cut in *visual* coordinates,
    // matching imgproxy. The fast path gets this for free from
    // vips_thumbnail_image; the slow path has to opt in. vips_autorot also
    // strips the orientation tag so the encoded output isn't rotated again by
    // viewers.
    {
        VipsImage* raw_rot = nullptr;
        int rr = vips_autorot(src.get(), &raw_rot, NULL);
        if (rr != 0 || !raw_rot) {
            std::string msg = vips_error_buffer();
            vips_error_clear();
            throw std::runtime_error("vips autorot failed: " + msg);
        }
        src = VipsImageGuard{raw_rot};
    }

    const int src_w = vips_image_get_width(src.get());
    const int src_h = vips_image_get_height(src.get());
    if (src_w <= 0 || src_h <= 0)
        throw std::runtime_error("vips crop: invalid source dimensions");

    const double sx    = static_cast<double>(w) / src_w;
    const double sy    = static_cast<double>(h) / src_h;
    const double scale = std::max(sx, sy);

    VipsImage* raw_scaled = nullptr;
    int r = vips_resize(src.get(), &raw_scaled, scale, NULL);
    if (r != 0 || !raw_scaled) {
        std::string msg = vips_error_buffer();
        vips_error_clear();
        throw std::runtime_error("vips resize (cover) failed: " + msg);
    }
    VipsImageGuard scaled{raw_scaled};

    const int scaled_w = vips_image_get_width(scaled.get());
    const int scaled_h = vips_image_get_height(scaled.get());

    // Window can't exceed the scaled canvas even under float rounding.
    const int win_w = std::min(w, scaled_w);
    const int win_h = std::min(h, scaled_h);

    int left = 0, top = 0;
    gravity_offset(gravity, fp_x, fp_y,
                   scaled_w, scaled_h, win_w, win_h,
                   left, top);

    VipsImage* raw_out = nullptr;
    r = vips_extract_area(scaled.get(), &raw_out, left, top, win_w, win_h, NULL);
    if (r != 0 || !raw_out) {
        std::string msg = vips_error_buffer();
        vips_error_clear();
        throw std::runtime_error("vips extract_area failed: " + msg);
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

// --- ingest (upload path) ---------------------------------------------------
// Validates, normalizes, optionally pre-resizes, strips metadata, and re-
// encodes the uploaded bytes so downstream never sees the original's EXIF /
// IPTC / XMP. Magic-byte validation relies on libvips' loader lookup —
// vips_foreign_find_load_buffer returns NULL for anything it can't open,
// which gives us format sniffing and forgery rejection in one call.
ImageProcessor::IngestResult ImageProcessor::ingest_sync(
    const std::vector<uint8_t>& buf, IngestOptions opts,
    int max_src_resolution_mp)
{
    if (buf.empty())
        throw HttpException(drogon::k400BadRequest, "empty upload body");

    // 1. Magic-byte validation. Rejects anything libvips can't decode —
    //    covers text/SVG/PDF/zip-bomb disguises sent with a fake image/*
    //    Content-Type.
    const char* loader = vips_foreign_find_load_buffer(buf.data(), buf.size());
    if (!loader) {
        vips_error_clear();
        throw HttpException(drogon::k415UnsupportedMediaType,
            "unsupported or unrecognized image format");
    }

    // 2. Detect the source format *before* loading, so we can apply the
    //    HEIC → JPEG normalization rule even on the fast path where we
    //    don't resize. sniff_source_format reads only the header bytes.
    const OutputFormat src_fmt = sniff_source_format(buf);

    // Resolve the target format:
    //   override_output wins; otherwise HEIC/AVIF from a camera roll is
    //   normalized to JPEG unless opts.normalize_heic is false; everything
    //   else is preserved in its source format.
    OutputFormat out_fmt = opts.override_output;
    if (out_fmt == OutputFormat::Auto) {
        if (opts.normalize_heic && src_fmt == OutputFormat::AVIF) {
            // sniff_source_format maps both HEIC and AVIF to AVIF; the
            // normalize rule treats "ISO-BMFF container" as the signal.
            out_fmt = OutputFormat::JPEG;
        } else {
            out_fmt = src_fmt;
        }
    }

    // 3. Load with random access — ingest applies vips_autorot (90°/180°/270°
    //    transpose) and optional resize, both of which need non-sequential
    //    pixel access. A sequential loader would fail later at save time with
    //    "VipsJpeg: out of order read". Memory is bounded by UPLOAD_MAX_BYTES.
    VipsImage* raw = vips_image_new_from_buffer(
        buf.data(), buf.size(), "", NULL);
    if (!raw) {
        std::string msg = vips_error_buffer(); vips_error_clear();
        throw std::runtime_error("vips load failed: " + msg);
    }
    VipsImageGuard img{raw};

    // 4. Resolution cap — same check as the serve path.
    check_src_resolution(img.get(), max_src_resolution_mp);

    // 5. Apply EXIF orientation on ingest so stored pixels are already in
    //    visual order. Downstream readers (and the serve path) can then
    //    ignore orientation entirely; vips_autorot also drops the tag.
    {
        VipsImage* rot = nullptr;
        if (vips_autorot(img.get(), &rot, NULL) != 0 || !rot) {
            std::string msg = vips_error_buffer(); vips_error_clear();
            throw std::runtime_error("vips autorot failed: " + msg);
        }
        img = VipsImageGuard{rot};
    }

    // 6. Optional pre-resize: cap the long edge to pre_resize_max_dim.
    //    Shrinks oversized uploads at ingest so we don't pay the decode
    //    cost twice per request on the serve path. Skipped when the source
    //    is already within the cap (no-enlarge passthrough).
    if (opts.pre_resize_max_dim > 0) {
        const int w = vips_image_get_width(img.get());
        const int h = vips_image_get_height(img.get());
        const int long_edge = std::max(w, h);
        if (long_edge > opts.pre_resize_max_dim) {
            const double scale =
                static_cast<double>(opts.pre_resize_max_dim) / long_edge;
            VipsImage* scaled = nullptr;
            if (vips_resize(img.get(), &scaled, scale, NULL) != 0 || !scaled) {
                std::string msg = vips_error_buffer(); vips_error_clear();
                throw std::runtime_error("vips pre-resize failed: " + msg);
            }
            img = VipsImageGuard{scaled};
        }
    }

    IngestResult res;
    res.output = out_fmt;
    res.width  = vips_image_get_width(img.get());
    res.height = vips_image_get_height(img.get());
    // save_image runs strip_sensitive_metadata before encoding, so the
    // output bytes are already scrubbed of EXIF-GPS / XMP / IPTC.
    res.bytes  = save_image(img.get(), out_fmt, opts.quality);
    return res;
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
    // See S3Client.cpp for the rationale — same pattern here. The worker
    // thread runs `job` on state->buf, then resumes via this guard so a
    // frame destroyed in flight is never touched again (fixes C1).
    struct CancelGuard {
        std::mutex m;
        bool       alive = true;
    };
    auto state = std::make_shared<State>();
    state->buf = std::move(buf);
    state->job = std::move(job);
    auto guard = std::make_shared<CancelGuard>();

    // C2: check queue-full BEFORE entering the awaiter. Throwing from
    // await_suspend is legal per C++20 (compiler resumes the coroutine with
    // the exception), but it's a notoriously fragile corner — some runtimes
    // have shipped bugs around it, and the contract differs subtly between
    // compilers. Rejecting here — before we ever suspend — propagates the
    // exception through Drogon's Task<> machinery via the normal coroutine
    // exit path.
    {
        std::unique_lock lock(mu_);
        if (max_queue_size_ > 0 &&
            static_cast<int>(queue_.size()) >= max_queue_size_)
        {
            lock.unlock();
            throw HttpException(drogon::k429TooManyRequests, "too many requests");
        }
    }

    struct Awaiter {
        ImageProcessor*              self;
        std::shared_ptr<State>       state;
        std::shared_ptr<CancelGuard> guard;
        trantor::EventLoop*          loop;

        ~Awaiter() {
            std::lock_guard lk(guard->m);
            guard->alive = false;
        }

        bool await_ready() { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            // No throwing path here: queue-full was rejected above, and
            // queue_.push / notify_one cannot throw in practice (std::deque
            // push may throw bad_alloc, but that aborts the whole request
            // cleanly via the coroutine's unhandled-exception handler — and
            // we still own the frame at this point because nothing external
            // has been told to resume us yet).
            {
                std::unique_lock lock(self->mu_);
                self->queue_.push([state = state, guard = guard, loop = loop, h]() mutable {
                    try   { state->result = state->job(state->buf); }
                    catch (...) { state->exc = std::current_exception(); }
                    auto resume_if_alive = [guard, h]() mutable {
                        std::unique_lock lk(guard->m);
                        if (!guard->alive) return;
                        lk.unlock();   // never resume under the lock — see S3Client.cpp
                        h.resume();
                    };
                    if (loop) loop->queueInLoop(std::move(resume_if_alive));
                    else       resume_if_alive();
                });
            }
            self->cv_.notify_one();
        }

        std::string await_resume() {
            if (state->exc) std::rethrow_exception(state->exc);
            return std::move(state->result);
        }
    };

    co_return co_await Awaiter{this, state, guard, loop};
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
    std::vector<uint8_t> buf, int w, int h, Gravity gravity,
    double fp_x, double fp_y, OutputFormat fmt, int quality)
{
    const int max_mp = max_src_resolution_mp_;
    co_return co_await submit(std::move(buf),
        [w, h, gravity, fp_x, fp_y, fmt, quality, max_mp](const std::vector<uint8_t>& b) {
            return crop_sync(b, w, h, gravity, fp_x, fp_y, fmt, quality, max_mp);
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

drogon::Task<ImageProcessor::IngestResult> ImageProcessor::ingest(
    std::vector<uint8_t> buf, IngestOptions opts)
{
    // submit() returns std::string; we smuggle the metadata out by writing
    // it to a shared result holder from inside the worker lambda, keeping
    // the submit API simple and avoiding a template instantiation on T.
    const int max_mp = max_src_resolution_mp_;
    auto meta = std::make_shared<IngestResult>();
    auto bytes = co_await submit(std::move(buf),
        [opts, meta, max_mp](const std::vector<uint8_t>& b) {
            auto r = ingest_sync(b, opts, max_mp);
            meta->output = r.output;
            meta->width  = r.width;
            meta->height = r.height;
            return std::move(r.bytes);
        });
    meta->bytes = std::move(bytes);
    co_return std::move(*meta);
}
