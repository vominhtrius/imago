#include "usecases/ResizeUseCase.hpp"
#include "infrastructure/s3/S3Client.hpp"
#include "infrastructure/vips/ImageProcessor.hpp"
#include "models/HttpException.hpp"
#include "utils/HttpUtils.hpp"

ResizeUseCase::ResizeUseCase(S3ClientWrapper* s3, ImageProcessor* processor)
    : s3_(s3), processor_(processor) {}

drogon::Task<drogon::HttpResponsePtr> ResizeUseCase::execute(ResizeRequest req) {
    auto data = co_await s3_->download(req.bucket, req.key);
    // Resolve "same as source" once the bytes are in hand; concrete format
    // drives both the encoder and the Content-Type header.
    req.output  = resolve_output_format(data, req.output);
    auto output = co_await processor_->resize(
        std::move(data), req.w, req.h, req.fit, req.output, req.quality);

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString(content_type_for(req.output));
    resp->setBody(std::move(output));
    co_return resp;
}
