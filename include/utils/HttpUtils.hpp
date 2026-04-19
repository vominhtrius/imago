#pragma once
#include "models/ImageRequest.hpp"
#include <drogon/HttpResponse.h>
#include <charconv>
#include <string>
#include <string_view>

inline std::string content_type_for(OutputFormat fmt) {
    switch (fmt) {
    case OutputFormat::WebP: return "image/webp";
    case OutputFormat::JPEG: return "image/jpeg";
    case OutputFormat::PNG:  return "image/png";
    case OutputFormat::AVIF: return "image/avif";
    }
    return "application/octet-stream";
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
    if (s.empty() || s == "webp")    { out = OutputFormat::WebP; return true; }
    if (s == "jpeg" || s == "jpg")   { out = OutputFormat::JPEG; return true; }
    if (s == "png")                  { out = OutputFormat::PNG;  return true; }
    if (s == "avif")                 { out = OutputFormat::AVIF; return true; }
    return false;
}
