#include "plugins/UsecasePlugin.hpp"
#include "plugins/S3Plugin.hpp"
#include "plugins/VipsPlugin.hpp"
#include <drogon/drogon.h>

void UsecasePlugin::initAndStart(const Json::Value&) {
    auto* s3p   = drogon::app().getPlugin<S3Plugin>();
    auto* vipsp = drogon::app().getPlugin<VipsPlugin>();

    resize_  = std::make_unique<ResizeUseCase>(&s3p->s3(), &vipsp->processor());
    crop_    = std::make_unique<CropUseCase>(&s3p->s3(), &vipsp->processor());
    convert_ = std::make_unique<ConvertUseCase>(&s3p->s3(), &vipsp->processor());
}

void UsecasePlugin::shutdown() {}
