#pragma once
#include "models/ImageRequest.hpp"
#include <drogon/drogon.h>
#include <string>

class S3ClientWrapper;
class ImageProcessor;

// Upload-path defaults, resolved once at plugin init from AppConfig::from_env.
// The controller layers per-request overrides (query params) on top of these.
// Kept out of AppConfig so the controller isn't tempted to re-read env vars on
// every inbound request.
struct UploadDefaults {
    std::string bucket;
    std::string key_prefix         = "uploads/";
    long long   max_bytes          = 25LL << 20;
    int         pre_resize_max_dim = 0;
    bool        normalize_heic     = true;
};

class UploadUseCase {
public:
    UploadUseCase(S3ClientWrapper* s3, ImageProcessor* processor,
                  UploadDefaults defaults);

    const UploadDefaults& defaults() const noexcept { return defaults_; }

    drogon::Task<drogon::HttpResponsePtr> execute(UploadRequest req);

private:
    S3ClientWrapper* s3_;
    ImageProcessor*  processor_;
    UploadDefaults   defaults_;
};
