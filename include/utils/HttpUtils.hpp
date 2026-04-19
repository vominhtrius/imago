#pragma once
#include "models/ImageRequest.hpp"
#include <drogon/HttpResponse.h>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

inline std::string content_type_for(OutputFormat fmt) {
    switch (fmt) {
    case OutputFormat::WebP: return "image/webp";
    case OutputFormat::JPEG: return "image/jpeg";
    case OutputFormat::PNG:  return "image/png";
    case OutputFormat::AVIF: return "image/avif";
    case OutputFormat::Auto: break;   // caller must resolve before responding
    }
    return "application/octet-stream";
}

// Sniff the container format from the raw buffer's magic bytes. Mirrors the
// imgproxy "preserve source format" default for `output=` unset. AVIF/HEIC
// detection uses the ISO BMFF `ftyp` brand (bytes 4..12 of an ISO container).
inline OutputFormat sniff_source_format(const std::vector<uint8_t>& buf) {
    if (buf.size() < 12) return OutputFormat::WebP;    // fallback — tiny/unknown
    const auto* p = buf.data();

    // JPEG: FF D8 FF
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return OutputFormat::JPEG;

    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return OutputFormat::PNG;

    // RIFF....WEBP
    if (p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' &&
        p[8] == 'W' && p[9] == 'E' && p[10] == 'B' && p[11] == 'P')
        return OutputFormat::WebP;

    // ISO BMFF (HEIC / AVIF): bytes 4..7 = "ftyp", brand at 8..11.
    if (p[4] == 'f' && p[5] == 't' && p[6] == 'y' && p[7] == 'p') {
        std::string_view brand{reinterpret_cast<const char*>(p + 8), 4};
        if (brand == "avif" || brand == "avis") return OutputFormat::AVIF;
        if (brand == "heic" || brand == "heix" || brand == "heim" ||
            brand == "heis" || brand == "mif1" || brand == "msf1")
            return OutputFormat::AVIF;   // transcode HEIC → AVIF (closest in our set)
    }

    return OutputFormat::WebP;   // unknown — fall back to WebP
}

// Resolve `Auto` against the source buffer; passes concrete formats through.
inline OutputFormat resolve_output_format(const std::vector<uint8_t>& buf,
                                          OutputFormat requested) {
    return requested == OutputFormat::Auto ? sniff_source_format(buf) : requested;
}

inline drogon::HttpResponsePtr bad_request(const std::string& msg) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k400BadRequest);
    resp->setContentTypeString("text/plain");
    resp->setBody(msg);
    return resp;
}

inline drogon::HttpResponsePtr error_response(int code, const std::string& msg) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(code));
    resp->setContentTypeString("text/plain");
    resp->setBody(msg);
    return resp;
}

inline bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return true;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size() && out >= 0;
}

inline bool parse_fit(const std::string& s, FitMode& out) {
    if (s.empty() || s == "fit")  { out = FitMode::Fit;      return true; }
    if (s == "fill")              { out = FitMode::Fill;     return true; }
    if (s == "fill-down")         { out = FitMode::FillDown; return true; }
    if (s == "force")             { out = FitMode::Force;    return true; }
    return false;
}

// Parses imgproxy-style gravity tokens plus imago extras. Writes focus-point
// coordinates into fp_x/fp_y when token is "fp:X:Y"; otherwise leaves them.
//
// Accepted tokens (case-sensitive):
//   ""                → Center (default)
//   "ce" | "center"                 → Center
//   "no" | "north"                  → North
//   "so" | "south"                  → South
//   "ea" | "east"                   → East
//   "we" | "west"                   → West
//   "noea" | "northeast"            → NorthEast
//   "nowe" | "northwest"            → NorthWest
//   "soea" | "southeast"            → SouthEast
//   "sowe" | "southwest"            → SouthWest
//   "sm" | "smart" | "attention"    → Smart (libvips ATTENTION)
//   "entropy"                       → Entropy (libvips ENTROPY, imago-only)
//   "fp:X:Y" with X,Y ∈ [0,1]       → FocusPoint, fp_x=X, fp_y=Y
inline bool parse_gravity(const std::string& s, Gravity& out,
                          double& fp_x, double& fp_y) {
    if (s.empty() || s == "ce" || s == "center") { out = Gravity::Center;    return true; }
    if (s == "no" || s == "north")               { out = Gravity::North;     return true; }
    if (s == "so" || s == "south")               { out = Gravity::South;     return true; }
    if (s == "ea" || s == "east")                { out = Gravity::East;      return true; }
    if (s == "we" || s == "west")                { out = Gravity::West;      return true; }
    if (s == "noea" || s == "northeast")         { out = Gravity::NorthEast; return true; }
    if (s == "nowe" || s == "northwest")         { out = Gravity::NorthWest; return true; }
    if (s == "soea" || s == "southeast")         { out = Gravity::SouthEast; return true; }
    if (s == "sowe" || s == "southwest")         { out = Gravity::SouthWest; return true; }
    if (s == "sm" || s == "smart" || s == "attention") { out = Gravity::Smart;   return true; }
    if (s == "entropy")                                 { out = Gravity::Entropy; return true; }

    // fp:X:Y
    if (s.size() > 3 && s[0] == 'f' && s[1] == 'p' && s[2] == ':') {
        auto colon = s.find(':', 3);
        if (colon == std::string::npos) return false;
        std::string xs = s.substr(3, colon - 3);
        std::string ys = s.substr(colon + 1);
        if (xs.empty() || ys.empty()) return false;
        try {
            double x = std::stod(xs);
            double y = std::stod(ys);
            if (!(x >= 0.0 && x <= 1.0) || !(y >= 0.0 && y <= 1.0)) return false;
            fp_x = x;
            fp_y = y;
            out  = Gravity::FocusPoint;
            return true;
        } catch (...) { return false; }
    }
    return false;
}

inline bool parse_output(const std::string& s, OutputFormat& out) {
    // Empty / "auto" → preserve source format; resolved in the use case
    // after the buffer is downloaded (see resolve_output_format).
    if (s.empty() || s == "auto")    { out = OutputFormat::Auto; return true; }
    if (s == "webp")                 { out = OutputFormat::WebP; return true; }
    if (s == "jpeg" || s == "jpg")   { out = OutputFormat::JPEG; return true; }
    if (s == "png")                  { out = OutputFormat::PNG;  return true; }
    if (s == "avif")                 { out = OutputFormat::AVIF; return true; }
    return false;
}
