#pragma once
#include "usecases/ResizeUseCase.hpp"
#include "usecases/CropUseCase.hpp"
#include "usecases/ConvertUseCase.hpp"
#include "usecases/UploadUseCase.hpp"
#include <drogon/plugins/Plugin.h>
#include <memory>

class UsecasePlugin : public drogon::Plugin<UsecasePlugin> {
public:
    void initAndStart(const Json::Value& config) override;
    void shutdown() override;

    ResizeUseCase&  resize()  { return *resize_; }
    CropUseCase&    crop()    { return *crop_; }
    ConvertUseCase& convert() { return *convert_; }
    UploadUseCase&  upload()  { return *upload_; }

private:
    std::unique_ptr<ResizeUseCase>  resize_;
    std::unique_ptr<CropUseCase>    crop_;
    std::unique_ptr<ConvertUseCase> convert_;
    std::unique_ptr<UploadUseCase>  upload_;
};
