#pragma once
#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

struct AppConfig {
    std::string aws_access_key_id;
    std::string aws_secret_access_key;
    std::string aws_region        = "us-east-1";
    std::string s3_endpoint_url;
    bool        s3_force_path_style = false;
    int         thread_pool_size    = static_cast<int>(std::thread::hardware_concurrency());
    int         s3_executor_threads = 8;
    int         drogon_workers      = static_cast<int>(std::thread::hardware_concurrency());
    int         port                = 8080;
    int         max_src_resolution_mp = 50;         // megapixels; 0 = disabled
    int         max_queue_size        = 0;          // vips job queue cap; 0 = unlimited
    long long   max_object_size_bytes = 128LL << 20; // S3 download cap; 128 MiB, 0 = unlimited

    // --- Upload (ingest) path --------------------------------------------
    // Enforced by UploadController / UploadUseCase; uploader rejects bodies
    // larger than upload_max_bytes before decode, and the ingest processor
    // bounds the pre-resize long edge to upload_pre_resize_max_dim (0 = no
    // pre-resize — store original decoded+stripped bytes).
    std::string upload_bucket;                         // required for uploads; empty → reject
    std::string upload_key_prefix       = "uploads/";  // trailing "/" preserved verbatim
    long long   upload_max_bytes        = 25LL << 20; // 25 MiB ingest cap, 0 = unlimited
    int         upload_pre_resize_max_dim = 0;         // long-edge px; 0 = keep source size
    bool        upload_normalize_heic   = true;        // HEIC → JPEG on ingest

    static AppConfig from_env() {
        AppConfig cfg;
        if (auto v = std::getenv("AWS_ACCESS_KEY_ID"))     cfg.aws_access_key_id     = v;
        if (auto v = std::getenv("AWS_SECRET_ACCESS_KEY")) cfg.aws_secret_access_key = v;
        if (auto v = std::getenv("AWS_REGION"))            cfg.aws_region            = v;
        if (auto v = std::getenv("S3_ENDPOINT_URL"))       cfg.s3_endpoint_url       = v;
        if (auto v = std::getenv("S3_FORCE_PATH_STYLE"))   cfg.s3_force_path_style   = (std::string(v) == "true");
        parse_env_int("THREAD_POOL_SIZE",       cfg.thread_pool_size);
        parse_env_int("S3_EXECUTOR_THREADS",    cfg.s3_executor_threads);
        parse_env_int("DROGON_WORKERS",         cfg.drogon_workers);
        parse_env_int("IMAGO_PORT",             cfg.port);
        parse_env_int("MAX_SRC_RESOLUTION_MP",  cfg.max_src_resolution_mp);
        parse_env_int("MAX_QUEUE_SIZE",         cfg.max_queue_size);
        parse_env_ll ("MAX_OBJECT_SIZE_BYTES",  cfg.max_object_size_bytes);

        if (auto v = std::getenv("UPLOAD_BUCKET"))      cfg.upload_bucket     = v;
        if (auto v = std::getenv("UPLOAD_KEY_PREFIX"))  cfg.upload_key_prefix = v;
        parse_env_ll ("UPLOAD_MAX_BYTES",        cfg.upload_max_bytes);
        parse_env_int("UPLOAD_PRE_RESIZE_MAX_DIM", cfg.upload_pre_resize_max_dim);
        if (auto v = std::getenv("UPLOAD_NORMALIZE_HEIC"))
            cfg.upload_normalize_heic = (std::string(v) == "true");
        return cfg;
    }

private:
    // std::from_chars is locale-independent and never throws; we surface a
    // specific message naming the offending variable instead of letting
    // std::stoi's opaque invalid_argument bubble up from main().
    static void parse_env_int(const char* name, int& out) {
        auto v = std::getenv(name);
        if (!v || *v == '\0') return;
        int tmp = 0;
        const auto* end = v + std::char_traits<char>::length(v);
        auto [ptr, ec] = std::from_chars(v, end, tmp);
        if (ec != std::errc{} || ptr != end)
            throw std::runtime_error(std::string("invalid int in env var ") + name + "=" + v);
        out = tmp;
    }
    static void parse_env_ll(const char* name, long long& out) {
        auto v = std::getenv(name);
        if (!v || *v == '\0') return;
        long long tmp = 0;
        const auto* end = v + std::char_traits<char>::length(v);
        auto [ptr, ec] = std::from_chars(v, end, tmp);
        if (ec != std::errc{} || ptr != end)
            throw std::runtime_error(std::string("invalid int in env var ") + name + "=" + v);
        out = tmp;
    }
};
