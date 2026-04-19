#include "controllers/ConvertController.hpp"
#include "plugins/UsecasePlugin.hpp"
#include "models/HttpException.hpp"
#include "utils/HttpUtils.hpp"
#include <drogon/drogon.h>

drogon::Task<drogon::HttpResponsePtr> ConvertController::handle(
    drogon::HttpRequestPtr req,
    std::string            bucket,
    std::string            key)
{
    if (bucket.empty() || key.empty())
        co_return bad_request("missing bucket or key");

    ConvertRequest img_req;
    img_req.bucket = std::move(bucket);
    img_req.key    = std::move(key);

    if (!parse_output(req->getParameter("output"), img_req.output))
        co_return bad_request("invalid 'output' (auto|webp|jpeg|png|avif)");
    if (!parse_int(req->getParameter("quality"), img_req.quality))
        co_return bad_request("invalid 'quality' (1-100)");

    try {
        co_return co_await drogon::app()
            .getPlugin<UsecasePlugin>()
            ->convert()
            .execute(std::move(img_req));
    } catch (const HttpException& e) {
        co_return error_response(e.status_code, e.message);
    } catch (const std::exception& e) {
        co_return error_response(500, e.what());
    }
}
