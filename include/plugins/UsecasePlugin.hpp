#pragma once
#include "usecases/ResizeUseCase.hpp"
#include "usecases/CropUseCase.hpp"
#include "usecases/ConvertUseCase.hpp"
#include <drogon/plugins/Plugin.h>
#include <memory>

class UsecasePlugin : public drogon::Plugin<UsecasePlugin> {
public:
    void initAndStart(const Json::Value& config) override;
    void shutdown() override;

    ResizeUseCase&  resize()  { return *resize_; }
    CropUseCase&    crop()    { return *crop_; }
    ConvertUseCase& convert() { return *convert_; }

private:
    std::unique_ptr<ResizeUseCase>  resize_;
    std::unique_ptr<CropUseCase>    crop_;
    std::unique_ptr<ConvertUseCase> convert_;
};
