#include "controllers/CropController.hpp"
#include "plugins/UsecasePlugin.hpp"
#include "models/HttpException.hpp"
#include "utils/HttpUtils.hpp"
#include <drogon/drogon.h>

drogon::Task<drogon::HttpResponsePtr> CropController::handle(
    drogon::HttpRequestPtr req,
    std::string            bucket,
    std::string            key)
{
    if (bucket.empty() || key.empty())
        co_return bad_request("missing bucket or key");

    CropRequest img_req;
    img_req.bucket = std::move(bucket);
    img_req.key    = std::move(key);

    if (!parse_dim(req->getParameter("w"), img_req.w) || img_req.w <= 0)
        co_return bad_request("'w' is required and must be in 1..16384");
    if (!parse_dim(req->getParameter("h"), img_req.h) || img_req.h <= 0)
        co_return bad_request("'h' is required and must be in 1..16384");
    if (!parse_gravity(req->getParameter("gravity"), img_req.gravity,
                       img_req.fp_x, img_req.fp_y))
        co_return bad_request(
            "invalid 'gravity' "
            "(ce|no|so|ea|we|noea|nowe|soea|sowe|sm|entropy|fp:X:Y)");
    if (!parse_output(req->getParameter("output"), img_req.output))
        co_return bad_request("invalid 'output' (auto|webp|jpeg|png|avif)");
    if (!parse_int(req->getParameter("quality"), img_req.quality))
        co_return bad_request("invalid 'quality' (1-100)");

    try {
        co_return co_await drogon::app()
            .getPlugin<UsecasePlugin>()
            ->crop()
            .execute(std::move(img_req));
    } catch (const HttpException& e) {
        co_return error_response(e.status_code, e.message);
    } catch (const std::exception& e) {
        co_return error_response(500, e.what());
    }
}
