#pragma once
#include <string>
#include <cstdlib>
#include <thread>

struct AppConfig {
    std::string aws_access_key_id;
    std::string aws_secret_access_key;
    std::string aws_region        = "us-east-1";
    std::string s3_endpoint_url;
    bool        s3_force_path_style = false;
    int         thread_pool_size    = static_cast<int>(std::thread::hardware_concurrency());
    int         s3_executor_threads = static_cast<int>(std::thread::hardware_concurrency()) * 2;
    int         drogon_workers      = static_cast<int>(std::thread::hardware_concurrency());
    int         port                = 8080;
    int         max_src_resolution_mp = 50; // megapixels; 0 = disabled
    int         max_queue_size        = 0;  // vips job queue cap; 0 = unlimited

    static AppConfig from_env() {
        AppConfig cfg;
        if (auto v = std::getenv("AWS_ACCESS_KEY_ID"))     cfg.aws_access_key_id     = v;
        if (auto v = std::getenv("AWS_SECRET_ACCESS_KEY")) cfg.aws_secret_access_key = v;
        if (auto v = std::getenv("AWS_REGION"))            cfg.aws_region            = v;
        if (auto v = std::getenv("S3_ENDPOINT_URL"))       cfg.s3_endpoint_url       = v;
        if (auto v = std::getenv("S3_FORCE_PATH_STYLE"))   cfg.s3_force_path_style   = (std::string(v) == "true");
        if (auto v = std::getenv("THREAD_POOL_SIZE"))      cfg.thread_pool_size      = std::stoi(v);
        if (auto v = std::getenv("S3_EXECUTOR_THREADS"))   cfg.s3_executor_threads   = std::stoi(v);
        if (auto v = std::getenv("DROGON_WORKERS"))        cfg.drogon_workers        = std::stoi(v);
        if (auto v = std::getenv("IMAGO_PORT"))                cfg.port                  = std::stoi(v);
        if (auto v = std::getenv("MAX_SRC_RESOLUTION_MP"))    cfg.max_src_resolution_mp = std::stoi(v);
        if (auto v = std::getenv("MAX_QUEUE_SIZE"))            cfg.max_queue_size        = std::stoi(v);
        return cfg;
    }
};
