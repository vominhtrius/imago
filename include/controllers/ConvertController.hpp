#pragma once
#include <drogon/HttpController.h>

class ConvertController : public drogon::HttpController<ConvertController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_VIA_REGEX(ConvertController::handle, "/convert/([^/]+)/(.*)", drogon::Get);
    METHOD_LIST_END

    drogon::Task<drogon::HttpResponsePtr> handle(
        drogon::HttpRequestPtr req,
        std::string bucket,
        std::string key);
};
