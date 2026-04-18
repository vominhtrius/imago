#pragma once
#include "infrastructure/s3/S3Client.hpp"
#include <drogon/plugins/Plugin.h>
#include <memory>

class S3Plugin : public drogon::Plugin<S3Plugin> {
public:
    void initAndStart(const Json::Value& config) override;
    void shutdown() override;

    S3ClientWrapper& s3() { return *s3_; }

private:
    std::unique_ptr<S3ClientWrapper> s3_;
};
