#pragma once
#include <drogon/HttpController.h>

class ResizeController : public drogon::HttpController<ResizeController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_VIA_REGEX(ResizeController::handle, "/resize/([^/]+)/(.*)", drogon::Get);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> handle(
        drogon::HttpRequestPtr req,
        std::string bucket,
        std::string key);
};
