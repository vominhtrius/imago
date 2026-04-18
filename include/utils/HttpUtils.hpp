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

inline bool parse_gravity(const std::string& s, Gravity& out) {
    if (s.empty() || s == "attention") { out = Gravity::Attention; return true; }
    if (s == "entropy")                { out = Gravity::Entropy;   return true; }
    if (s == "center")                 { out = Gravity::Center;    return true; }
    return false;
}

inline bool parse_output(const std::string& s, OutputFormat& out) {
    if (s.empty() || s == "webp")    { out = OutputFormat::WebP; return true; }
    if (s == "jpeg" || s == "jpg")   { out = OutputFormat::JPEG; return true; }
    if (s == "png")                  { out = OutputFormat::PNG;  return true; }
    if (s == "avif")                 { out = OutputFormat::AVIF; return true; }
    return false;
}
