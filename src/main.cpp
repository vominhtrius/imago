#include "config/AppConfig.hpp"
#include <drogon/drogon.h>
#include <aws/core/Aws.h>
#include <vips/vips.h>
#include <chrono>
#include <cstdio>
#include <iostream>

int main() {
    setenv("AWS_EC2_METADATA_DISABLED", "true", 0);

    Aws::SDKOptions aws_opts;
    Aws::InitAPI(aws_opts);

    if (VIPS_INIT("imago")) vips_error_exit(nullptr);
    vips_cache_set_max_mem(0);
    vips_cache_set_max(0);
    vips_concurrency_set(1);

#ifdef IMAGO_DEBUG
    vips_leak_set(TRUE);
#endif

    auto cfg = AppConfig::from_env();

    std::cerr << "imago starting on port " << cfg.port
              << " | io_threads="     << cfg.drogon_workers
              << " | vips_pool="      << cfg.thread_pool_size
              << " | s3_executor="    << cfg.s3_executor_threads
              << " | hw_concurrency=" << std::thread::hardware_concurrency()
              << std::endl;

    // Register plugins via inline JSON — S3Plugin and VipsPlugin have no
    // dependencies; UsecasePlugin depends on both and is initialized last.
    Json::Value plugins(Json::arrayValue);
    auto add_plugin = [&](const char* name, std::initializer_list<const char*> deps) {
        Json::Value p;
        p["name"] = name;
        Json::Value d(Json::arrayValue);
        for (const auto* dep : deps) d.append(dep);
        p["dependencies"] = d;
        plugins.append(p);
    };
    add_plugin("S3Plugin",      {});
    add_plugin("VipsPlugin",    {});
    add_plugin("UsecasePlugin", {"S3Plugin", "VipsPlugin"});

    Json::Value drogon_cfg;
    drogon_cfg["plugins"] = plugins;

    // Record a monotonic start timestamp on every inbound request, then
    // emit it as `Server-Timing: imago;dur=<ms>` on the response so clients
    // (and browser devtools) can see the server-side processing time.
    // Same-origin in our setup, but we also expose the header via CORS so
    // direct cross-origin fetches can read it.
    // Drogon's default client_max_body_size is 1 MiB — too small for the
    // upload endpoint. Size the ceiling a few KiB above UPLOAD_MAX_BYTES so
    // multipart framing overhead doesn't trip the guard before the
    // controller can return a friendly 413 with the configured limit.
    const std::size_t client_body_cap =
        cfg.upload_max_bytes > 0
            ? static_cast<std::size_t>(cfg.upload_max_bytes) + (64 * 1024)
            : static_cast<std::size_t>(32) * 1024 * 1024;

    drogon::app()
        .loadConfigJson(drogon_cfg)
        .setThreadNum(cfg.drogon_workers)
        .addListener("0.0.0.0", cfg.port)
        .setMaxConnectionNum(2048)
        .setClientMaxBodySize(client_body_cap)
        .setClientMaxMemoryBodySize(client_body_cap)
        .registerPreRoutingAdvice([](const drogon::HttpRequestPtr& req) {
            req->attributes()->insert(
                "imago_start", std::chrono::steady_clock::now());
        })
        .registerPreSendingAdvice(
            [](const drogon::HttpRequestPtr& req,
               const drogon::HttpResponsePtr& resp) {
                auto attrs = req->attributes();
                if (!attrs->find("imago_start")) return;
                auto start = attrs->get<std::chrono::steady_clock::time_point>(
                    "imago_start");
                auto ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                char buf[64];
                std::snprintf(buf, sizeof(buf), "imago;dur=%.2f", ms);
                resp->addHeader("Server-Timing", buf);
                resp->addHeader("Access-Control-Expose-Headers",
                                "Server-Timing");
            })
        .registerBeginningAdvice([] {
            std::cerr << "Drogon event loop started, listening..." << std::endl;
        })
        .run();

    std::cerr << "Drogon exited." << std::endl;

    vips_shutdown();
    Aws::ShutdownAPI(aws_opts);
    return 0;
}
