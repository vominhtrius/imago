#pragma once
#include "models/ImageRequest.hpp"
#include <drogon/drogon.h>

class S3ClientWrapper;
class ImageProcessor;

class ConvertUseCase {
public:
    ConvertUseCase(S3ClientWrapper* s3, ImageProcessor* processor);
    drogon::Task<drogon::HttpResponsePtr> execute(ConvertRequest req);

private:
    S3ClientWrapper* s3_;
    ImageProcessor*  processor_;
};
