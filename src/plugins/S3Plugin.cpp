#include "plugins/S3Plugin.hpp"
#include "config/AppConfig.hpp"

void S3Plugin::initAndStart(const Json::Value&) {
    s3_ = std::make_unique<S3ClientWrapper>(AppConfig::from_env());
}

void S3Plugin::shutdown() {}
