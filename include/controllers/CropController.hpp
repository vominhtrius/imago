#pragma once
#include <drogon/HttpController.h>

class CropController : public drogon::HttpController<CropController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_VIA_REGEX(CropController::handle, "/crop/([^/]+)/(.*)", drogon::Get);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> handle(
        drogon::HttpRequestPtr req,
        std::string bucket,
        std::string key);
};
