#pragma once
#include <drogon/HttpController.h>

// POST /v1/upload
//   Body:   raw image bytes OR multipart/form-data with a "file" field.
//   Query:  ?prefix=<s3-prefix>    override upload_key_prefix
//           ?bucket=<bucket>       override upload_bucket (admin only)
//           ?normalize_heic=false  disable HEIC→JPEG normalization
//           ?pre_resize=<px>       cap long edge (px) before storing
//           ?output=<fmt>          force output format (auto|webp|jpeg|png|avif)
//           ?quality=<1-100>       encoder quality override
//   Response: 201 Created, JSON { bucket, key, url, content_type, bytes, width, height }
class UploadController : public drogon::HttpController<UploadController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UploadController::handle, "/v1/upload", drogon::Post, drogon::Options);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> handle(drogon::HttpRequestPtr req);
};
