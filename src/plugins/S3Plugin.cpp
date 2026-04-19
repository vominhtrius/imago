#include "plugins/S3Plugin.hpp"
#include "config/AppConfig.hpp"

void S3Plugin::initAndStart(const Json::Value&) {
    s3_ = std::make_unique<S3ClientWrapper>(AppConfig::from_env());
}

void S3Plugin::shutdown() {
    // Drain the AWS executor before Aws::ShutdownAPI runs in main().
    // Drogon calls this while the event loop is still alive, so any
    // in-flight GetObjectAsync callback that wants to queueInLoop a
    // resume can still do so — and it sees the awaiter's guard
    // already flipped to alive=false by the frame teardown path,
    // so resume is skipped safely.
    if (s3_) s3_->shutdown();
}
