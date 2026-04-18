#pragma once
#include "models/ImageRequest.hpp"
#include <drogon/drogon.h>

class S3ClientWrapper;
class ImageProcessor;

class CropUseCase {
public:
    CropUseCase(S3ClientWrapper* s3, ImageProcessor* processor);
    drogon::Task<drogon::HttpResponsePtr> execute(CropRequest req);

private:
    S3ClientWrapper* s3_;
    ImageProcessor*  processor_;
};
