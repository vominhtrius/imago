#include "usecases/ConvertUseCase.hpp"
#include "infrastructure/s3/S3Client.hpp"
#include "infrastructure/vips/ImageProcessor.hpp"
#include "models/HttpException.hpp"
#include "utils/HttpUtils.hpp"

ConvertUseCase::ConvertUseCase(S3ClientWrapper* s3, ImageProcessor* processor)
    : s3_(s3), processor_(processor) {}

drogon::Task<drogon::HttpResponsePtr> ConvertUseCase::execute(ConvertRequest req) {
    auto data   = co_await s3_->download(req.bucket, req.key);
    auto output = co_await processor_->convert(std::move(data), req.output, req.quality);

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString(content_type_for(req.output));
    resp->setBody(std::move(output));
    co_return resp;
}
