#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class FitMode    { Fit, Fill, FillDown, Force };
enum class Gravity {
    // Cardinal / corner / center — imgproxy parity
    Center,
    North, South, East, West,
    NorthEast, NorthWest, SouthEast, SouthWest,
    // Content-aware
    Smart,       // libvips ATTENTION (imgproxy "sm")
    Entropy,     // libvips ENTROPY (imago extra, not in imgproxy)
    // Focus point — fp_x, fp_y carried in CropRequest
    FocusPoint,
};
// `Auto` = "same as source". Use cases resolve it to a concrete format by
// peeking the downloaded buffer's magic bytes before dispatching to the
// processor. The encoder path never sees `Auto`.
enum class OutputFormat { Auto, WebP, JPEG, PNG, AVIF };

struct ResizeRequest {
    std::string  bucket;
    std::string  key;
    int          w       = 0;
    int          h       = 0;
    int          quality = -1;   // -1 = use per-format default
    FitMode      fit     = FitMode::Fit;
    OutputFormat output  = OutputFormat::Auto;
};

struct CropRequest {
    std::string  bucket;
    std::string  key;
    int          w       = 0;
    int          h       = 0;
    int          quality = -1;
    Gravity      gravity = Gravity::Center;
    double       fp_x    = 0.5;   // focus-point X in [0,1], used when gravity == FocusPoint
    double       fp_y    = 0.5;   // focus-point Y in [0,1]
    OutputFormat output  = OutputFormat::Auto;
};

struct ConvertRequest {
    std::string  bucket;
    std::string  key;
    int          quality = -1;
    OutputFormat output  = OutputFormat::Auto;
};

// Upload (ingest) request. `data` is the raw, as-received body — binary or
// extracted from multipart. The ingest pipeline sniffs magic bytes, rejects
// forged content types, strips metadata, optionally normalizes HEIC, and
// optionally pre-resizes to cap long-edge dimension.
struct UploadRequest {
    std::vector<uint8_t> data;
    std::string          bucket;
    std::string          key_prefix;        // destination prefix, inc. trailing "/"
    std::string          declared_content_type;  // from request header; advisory only
    long long            max_bytes           = 0;  // 0 = rely on global cap
    int                  pre_resize_max_dim  = 0;  // 0 = keep source dimensions
    bool                 normalize_heic      = true;
    OutputFormat         override_output     = OutputFormat::Auto; // Auto = keep source format
    int                  quality             = -1; // -1 = format default
};

