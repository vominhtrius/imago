#include "controllers/ResizeController.hpp"
#include "plugins/UsecasePlugin.hpp"
#include "models/HttpException.hpp"
#include "utils/HttpUtils.hpp"
#include <drogon/drogon.h>

drogon::Task<drogon::HttpResponsePtr> ResizeController::handle(
    drogon::HttpRequestPtr req,
    std::string            bucket,
    std::string            key)
{
    if (bucket.empty() || key.empty())
        co_return bad_request("missing bucket or key");

    ResizeRequest img_req;
    img_req.bucket = std::move(bucket);
    img_req.key    = std::move(key);

    if (!parse_int(req->getParameter("w"), img_req.w))
        co_return bad_request("invalid 'w'");
    if (!parse_int(req->getParameter("h"), img_req.h))
        co_return bad_request("invalid 'h'");
    if (img_req.w <= 0 && img_req.h <= 0)
        co_return bad_request("'w' or 'h' is required (use /convert to only transcode)");
    if (!parse_fit(req->getParameter("fit"), img_req.fit))
        co_return bad_request("invalid 'fit' (fit|fill|fill-down|force)");
    if (!parse_output(req->getParameter("output"), img_req.output))
        co_return bad_request("invalid 'output' (webp|jpeg|png|avif)");
    if (!parse_int(req->getParameter("quality"), img_req.quality))
        co_return bad_request("invalid 'quality' (1-100)");

    try {
        co_return co_await drogon::app()
            .getPlugin<UsecasePlugin>()
            ->resize()
            .execute(std::move(img_req));
    } catch (const HttpException& e) {
        co_return error_response(e.status_code, e.message);
    } catch (const std::exception& e) {
        co_return error_response(500, e.what());
    }
}
