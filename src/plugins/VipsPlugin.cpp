#include "plugins/VipsPlugin.hpp"
#include "config/AppConfig.hpp"

void VipsPlugin::initAndStart(const Json::Value&) {
    auto cfg = AppConfig::from_env();
    processor_ = std::make_unique<ImageProcessor>(cfg.thread_pool_size, cfg.max_src_resolution_mp, cfg.max_queue_size);
}

void VipsPlugin::shutdown() {}
