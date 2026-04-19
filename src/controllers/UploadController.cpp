#include "controllers/UploadController.hpp"
#include "plugins/UsecasePlugin.hpp"
#include "config/AppConfig.hpp"
#include "models/HttpException.hpp"
#include "utils/HttpUtils.hpp"
#include <drogon/MultiPart.h>
#include <drogon/drogon.h>
#include <string>
#include <string_view>

namespace {

// Extract the request body. For multipart/form-data we accept the first
// file part (any field name) — this keeps the endpoint usable from plain
// <input type="file"> forms without forcing a specific field name on
// callers. Binary bodies (Content-Type: image/*, application/octet-stream,
// or missing) are taken verbatim.
bool extract_body(const drogon::HttpRequestPtr& req,
                  std::vector<uint8_t>& out,
                  std::string& content_type_out) {
    const auto ct = req->getHeader("Content-Type");
    if (ct.find("multipart/form-data") != std::string::npos) {
        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0) return false;
        const auto& files = parser.getFiles();
        if (files.empty()) return false;
        const auto& f = files.front();
        const auto sv = f.fileContent();
        out.assign(reinterpret_cast<const uint8_t*>(sv.data()),
                   reinterpret_cast<const uint8_t*>(sv.data() + sv.size()));
        // declared_content_type is advisory only — the ingest pipeline
        // sniffs magic bytes and rejects forgeries regardless, so we don't
        // bother translating drogon's ContentType enum back to a MIME
        // string here. Leave empty; the response's content_type comes from
        // what libvips actually decoded.
        content_type_out.clear();
        return true;
    }
    // Raw body path.
    const auto body = req->body();   // std::string_view
    out.assign(reinterpret_cast<const uint8_t*>(body.data()),
               reinterpret_cast<const uint8_t*>(body.data() + body.size()));
    content_type_out = std::string(ct);
    return true;
}

drogon::HttpResponsePtr preflight() {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
    resp->addHeader("Access-Control-Max-Age", "600");
    return resp;
}

}  // namespace

drogon::Task<drogon::HttpResponsePtr> UploadController::handle(
    drogon::HttpRequestPtr req)
{
    if (req->getMethod() == drogon::Options) co_return preflight();

    const auto cfg = AppConfig::from_env();

    // Bucket: query param override is allowed (useful for multi-tenant
    // deployments where the uploader handles several backing buckets).
    // In production, the operator console would sign a JWT bearing the
    // bucket claim — for now we accept the query param or fall through
    // to the configured default.
    std::string bucket = req->getParameter("bucket");
    if (bucket.empty()) bucket = cfg.upload_bucket;
    if (bucket.empty())
        co_return bad_request("upload bucket not configured "
                              "(set UPLOAD_BUCKET or ?bucket=)");

    std::string prefix = req->getParameter("prefix");
    if (prefix.empty()) prefix = cfg.upload_key_prefix;

    UploadRequest ureq;
    ureq.bucket             = std::move(bucket);
    ureq.key_prefix         = std::move(prefix);
    ureq.max_bytes          = cfg.upload_max_bytes;
    ureq.pre_resize_max_dim = cfg.upload_pre_resize_max_dim;
    ureq.normalize_heic     = cfg.upload_normalize_heic;

    if (auto v = req->getParameter("pre_resize"); !v.empty()) {
        int n = 0;
        if (!parse_dim(v, n) || n < 0)
            co_return bad_request("invalid 'pre_resize' (0..16384)");
        ureq.pre_resize_max_dim = n;
    }
    if (auto v = req->getParameter("normalize_heic"); !v.empty())
        ureq.normalize_heic = (v == "true" || v == "1");
    if (!parse_output(req->getParameter("output"), ureq.override_output))
        co_return bad_request("invalid 'output' (auto|webp|jpeg|png|avif)");
    if (!parse_int(req->getParameter("quality"), ureq.quality))
        co_return bad_request("invalid 'quality' (1-100)");

    if (!extract_body(req, ureq.data, ureq.declared_content_type))
        co_return bad_request("could not read request body");

    // Size check before the libvips worker pool — reject oversized payloads
    // without paying a queue slot. The usecase repeats the same check as a
    // safety net for callers that skip the controller.
    if (ureq.max_bytes > 0 &&
        static_cast<long long>(ureq.data.size()) > ureq.max_bytes)
    {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k413RequestEntityTooLarge);
        resp->setContentTypeString("text/plain");
        resp->setBody("upload exceeds UPLOAD_MAX_BYTES");
        co_return resp;
    }

    try {
        co_return co_await drogon::app()
            .getPlugin<UsecasePlugin>()
            ->upload()
            .execute(std::move(ureq));
    } catch (const HttpException& e) {
        co_return error_response(e.status_code, e.message);
    } catch (const std::exception& e) {
        co_return error_response(500, e.what());
    }
}
