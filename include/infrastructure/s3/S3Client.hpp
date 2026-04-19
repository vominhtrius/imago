#pragma once
#include "config/AppConfig.hpp"
#include "models/HttpException.hpp"
#include <drogon/drogon.h>
#include <aws/s3/S3Client.h>
#include <vector>
#include <cstdint>
#include <string>
#include <memory>

// Wraps AWS SDK S3Client with a co_await-friendly download interface.
// GetObjectAsync fires the callback on an AWS SDK executor thread pool
// (size = hardware_concurrency * 2); the coroutine resumes on the
// originating Drogon event loop thread via queueInLoop.
class S3ClientWrapper {
public:
    explicit S3ClientWrapper(const AppConfig& cfg);

    drogon::Task<std::vector<uint8_t>> download(
        const std::string& bucket, const std::string& key);

    // Drops the AWS S3 client (and the PooledThreadExecutor it owns,
    // whose destructor joins worker threads). Must run before
    // Aws::ShutdownAPI so that any in-flight callback has completed
    // before the SDK is torn down.
    void shutdown();

private:
    std::shared_ptr<Aws::S3::S3Client> client_;
    long long                          max_object_size_bytes_ = 0;   // 0 = unlimited
};
