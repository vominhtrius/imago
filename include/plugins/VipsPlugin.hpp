#pragma once
#include "infrastructure/vips/ImageProcessor.hpp"
#include <drogon/plugins/Plugin.h>
#include <memory>

class VipsPlugin : public drogon::Plugin<VipsPlugin> {
public:
    void initAndStart(const Json::Value& config) override;
    void shutdown() override;

    ImageProcessor& processor() { return *processor_; }

private:
    std::unique_ptr<ImageProcessor> processor_;
};
