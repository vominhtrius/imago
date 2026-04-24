#include "usecases/UploadUseCase.hpp"
#include "infrastructure/s3/S3Client.hpp"
#include "infrastructure/vips/ImageProcessor.hpp"
#include "models/HttpException.hpp"
#include "utils/HttpUtils.hpp"
#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <string>

UploadUseCase::UploadUseCase(S3ClientWrapper* s3, ImageProcessor* processor,
                             UploadDefaults defaults)
    : s3_(s3), processor_(processor), defaults_(std::move(defaults)) {}

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

// Normalize a client-supplied prefix:
//   * strip leading slashes so the key is always bucket-relative
//   * drop '.', '..', and empty path components — S3 keys are flat, but the
//     key is echoed in the Location header and in `s3://` URLs. A '..'
//     component survives there literally and browsers applying relative-URL
//     resolution would escape the intended folder.
//   * ensure exactly one trailing slash so the random suffix doesn't fuse
//     into a preceding directory name.
std::string normalize_prefix(std::string p) {
    std::string out;
    std::size_t start = 0;
    while (start < p.size()) {
        std::size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string_view seg(p.data() + start, end - start);
        if (!seg.empty() && seg != "." && seg != "..") {
            if (!out.empty()) out.push_back('/');
            out.append(seg);
        }
        start = end + 1;
    }
    if (!out.empty()) out.push_back('/');
    return out;
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

    // Move the encoded bytes straight into the uploader — save_image returns
    // std::string, S3ClientWrapper::upload accepts std::string, so there is
    // no intermediate copy of the payload (was a full 25 MiB memcpy per
    // request at the cap).
    const std::size_t stored_bytes = res.bytes.size();
    co_await s3_->upload(req.bucket, key, std::move(res.bytes), ct);

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
