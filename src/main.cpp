#include "config/AppConfig.hpp"
#include <drogon/drogon.h>
#include <aws/core/Aws.h>
#include <vips/vips.h>
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

    drogon::app()
        .loadConfigJson(drogon_cfg)
        .setThreadNum(cfg.drogon_workers)
        .addListener("0.0.0.0", cfg.port)
        .setMaxConnectionNum(2048)
        .registerBeginningAdvice([] {
            std::cerr << "Drogon event loop started, listening..." << std::endl;
        })
        .run();

    std::cerr << "Drogon exited." << std::endl;

    vips_shutdown();
    Aws::ShutdownAPI(aws_opts);
    return 0;
}
