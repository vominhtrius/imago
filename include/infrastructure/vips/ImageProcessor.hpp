#pragma once
#include "VipsImage.hpp"
#include "models/ImageRequest.hpp"
#include <drogon/drogon.h>
#include <functional>
#include <vector>
#include <cstdint>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>

// Runs libvips operations on a dedicated fixed-size thread pool.
// All three public methods dispatch to the pool via a shared coroutine
// awaiter and resume on the originating Drogon event loop thread.
class ImageProcessor {
public:
    explicit ImageProcessor(int workers, int max_src_resolution_mp = 50, int max_queue_size = 0);
    ~ImageProcessor();

    ImageProcessor(const ImageProcessor&)            = delete;
    ImageProcessor& operator=(const ImageProcessor&) = delete;

    drogon::Task<std::string> resize(
        std::vector<uint8_t> buf, int w, int h, FitMode fit, OutputFormat fmt, int quality = -1);

    drogon::Task<std::string> crop(
        std::vector<uint8_t> buf, int w, int h, Gravity gravity,
        double fp_x, double fp_y, OutputFormat fmt, int quality = -1);

    drogon::Task<std::string> convert(
        std::vector<uint8_t> buf, OutputFormat fmt, int quality = -1);

private:
    drogon::Task<std::string> submit(
        std::vector<uint8_t> buf,
        std::function<std::string(const std::vector<uint8_t>&)> job);

    static std::string save_image(VipsImage* img, OutputFormat fmt, int quality);

    static std::string resize_sync(
        const std::vector<uint8_t>& buf, int w, int h, FitMode fit, OutputFormat fmt, int quality,
        int max_src_resolution_mp);

    static std::string crop_sync(
        const std::vector<uint8_t>& buf, int w, int h, Gravity gravity,
        double fp_x, double fp_y, OutputFormat fmt, int quality,
        int max_src_resolution_mp);

    static std::string convert_sync(
        const std::vector<uint8_t>& buf, OutputFormat fmt, int quality,
        int max_src_resolution_mp);

    void worker_loop();

    int                                    max_src_resolution_mp_;
    int                                    max_queue_size_;
    std::vector<std::thread>              threads_;
    std::queue<std::function<void()>>     queue_;
    std::mutex                            mu_;
    std::condition_variable               cv_;
    bool                                  stop_ = false;
};
