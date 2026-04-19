#include "usecases/UploadUseCase.hpp"
#include "infrastructure/s3/S3Client.hpp"
#include "infrastructure/vips/ImageProcessor.hpp"
#include "models/HttpException.hpp"
#include "utils/HttpUtils.hpp"
#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <string>

UploadUseCase::UploadUseCase(S3ClientWrapper* s3, ImageProcessor* processor)
    : s3_(s3), processor_(processor) {}

namespace {

// Map the chosen encoder format to the file extension we write into the
// S3 key. Keys are visible in logs/URLs, so we use the conventional `.jpg`
// (not `.jpeg`) for parity with imgproxy's output.
std::string extension_for(OutputFormat fmt) {
    switch (fmt) {
    case OutputFormat::WebP: return ".webp";
    case OutputFormat::JPEG: return ".jpg";
    case OutputFormat::PNG:  return ".png";
    case OutputFormat::AVIF: return ".avif";
    case OutputFormat::Auto: break;
    }
    return ".bin";
}

// Normalize a client-supplied prefix: strip a leading slash so the key is
// always bucket-relative, and ensure a trailing slash so the random suffix
// doesn't fuse into a preceding directory name.
std::string normalize_prefix(std::string p) {
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    if (!p.empty() && p.back() != '/') p.push_back('/');
    return p;
}

}  // namespace

drogon::Task<drogon::HttpResponsePtr> UploadUseCase::execute(UploadRequest req) {
    if (req.bucket.empty())
        throw HttpException(500, "upload bucket not configured");
    if (req.data.empty())
        throw HttpException(400, "empty upload body");
    if (req.max_bytes > 0 &&
        static_cast<long long>(req.data.size()) > req.max_bytes)
        throw HttpException(413, "upload exceeds max_bytes");

    // Hand the bytes to the libvips pool — validation + strip + normalize +
    // optional pre-resize + re-encode all happen off the IO thread. Throws
    // 415 on unrecognized magic bytes, 413 on over-resolution sources.
    ImageProcessor::IngestOptions opts;
    opts.override_output    = req.override_output;
    opts.normalize_heic     = req.normalize_heic;
    opts.pre_resize_max_dim = req.pre_resize_max_dim;
    opts.quality            = req.quality;
    auto res = co_await processor_->ingest(std::move(req.data), opts);

    // Random 24-char alphanumeric key — ample entropy to preclude collisions
    // (62^24 ≈ 2^142), cheaper than hashing the full bytes. Design doc's
    // "URL always resolves to the exact bytes" invariant holds because keys
    // are never overwritten.
    const std::string prefix = normalize_prefix(std::move(req.key_prefix));
    const std::string key    = prefix + drogon::utils::genRandomString(24)
                              + extension_for(res.output);
    const std::string ct     = content_type_for(res.output);

    std::vector<uint8_t> body(res.bytes.begin(), res.bytes.end());
    const std::size_t stored_bytes = body.size();
    co_await s3_->upload(req.bucket, key, std::move(body), ct);

    Json::Value j;
    j["bucket"]       = req.bucket;
    j["key"]          = key;
    j["url"]          = "s3://" + req.bucket + "/" + key;
    j["content_type"] = ct;
    j["bytes"]        = static_cast<Json::UInt64>(stored_bytes);
    j["width"]        = res.width;
    j["height"]       = res.height;

    auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
    resp->setStatusCode(drogon::k201Created);
    resp->addHeader("Location", "/v1/upload/" + req.bucket + "/" + key);
    co_return resp;
}
