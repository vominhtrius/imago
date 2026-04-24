#include "plugins/UsecasePlugin.hpp"
#include "plugins/S3Plugin.hpp"
#include "plugins/VipsPlugin.hpp"
#include "config/AppConfig.hpp"
#include <drogon/drogon.h>

void UsecasePlugin::initAndStart(const Json::Value&) {
    auto* s3p   = drogon::app().getPlugin<S3Plugin>();
    auto* vipsp = drogon::app().getPlugin<VipsPlugin>();

    // Read upload-path defaults from env once here (same pattern as
    // S3Plugin/VipsPlugin). The controller used to call AppConfig::from_env()
    // on every inbound request, which meant one getenv() per key per request
    // and inconsistent behaviour if the operator changed env mid-run.
    const auto cfg = AppConfig::from_env();
    UploadDefaults ud;
    ud.bucket             = cfg.upload_bucket;
    ud.key_prefix         = cfg.upload_key_prefix;
    ud.max_bytes          = cfg.upload_max_bytes;
    ud.pre_resize_max_dim = cfg.upload_pre_resize_max_dim;
    ud.normalize_heic     = cfg.upload_normalize_heic;

    resize_  = std::make_unique<ResizeUseCase>(&s3p->s3(), &vipsp->processor());
    crop_    = std::make_unique<CropUseCase>(&s3p->s3(), &vipsp->processor());
    convert_ = std::make_unique<ConvertUseCase>(&s3p->s3(), &vipsp->processor());
    upload_  = std::make_unique<UploadUseCase>(
        &s3p->s3(), &vipsp->processor(), std::move(ud));
}

void UsecasePlugin::shutdown() {}
