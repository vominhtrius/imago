#include "infrastructure/s3/S3Client.hpp"
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/utils/threading/PooledThreadExecutor.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/S3Errors.h>
#include <trantor/net/EventLoop.h>
#include <sstream>

S3ClientWrapper::S3ClientWrapper(const AppConfig& cfg) {
    Aws::Client::ClientConfiguration aws_cfg;
    aws_cfg.region         = cfg.aws_region;
    aws_cfg.maxConnections = 128;
    aws_cfg.executor       = std::make_shared<Aws::Utils::Threading::PooledThreadExecutor>(
        cfg.s3_executor_threads);

    if (!cfg.s3_endpoint_url.empty()) {
        aws_cfg.endpointOverride = cfg.s3_endpoint_url;
        aws_cfg.scheme           = Aws::Http::Scheme::HTTP;
    }

    Aws::Auth::AWSCredentials creds(cfg.aws_access_key_id, cfg.aws_secret_access_key);
    Aws::S3::S3ClientConfiguration s3_cfg(aws_cfg);
    if (cfg.s3_force_path_style)
        s3_cfg.useVirtualAddressing = false;

    client_ = std::make_shared<Aws::S3::S3Client>(creds, nullptr, s3_cfg);
}

drogon::Task<std::vector<uint8_t>> S3ClientWrapper::download(
    const std::string& bucket, const std::string& key)
{
    auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();

    struct S3State {
        std::vector<uint8_t> buffer;
        std::string          error_message;
        int                  error_http_code = 500;
    };
    auto state = std::make_shared<S3State>();

    struct S3Awaiter {
        Aws::S3::S3Client*       client;
        std::string              bucket;
        std::string              key;
        trantor::EventLoop*      loop;
        std::shared_ptr<S3State> state;

        bool await_ready() { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            Aws::S3::Model::GetObjectRequest req;
            req.SetBucket(bucket);
            req.SetKey(key);

            client->GetObjectAsync(req,
                [state = state, loop = loop, h](
                    const Aws::S3::S3Client*,
                    const Aws::S3::Model::GetObjectRequest&,
                    Aws::S3::Model::GetObjectOutcome&& outcome,
                    const std::shared_ptr<const Aws::Client::AsyncCallerContext>&)
                {
                    if (outcome.IsSuccess()) {
                        auto& result = outcome.GetResult();
                        auto& body   = result.GetBody();
                        const long long len = result.GetContentLength();
                        if (len > 0) {
                            state->buffer.resize(static_cast<size_t>(len));
                            size_t total = 0;
                            while (total < static_cast<size_t>(len) && body) {
                                body.read(
                                    reinterpret_cast<char*>(state->buffer.data() + total),
                                    static_cast<std::streamsize>(len - total));
                                const auto got = body.gcount();
                                if (got <= 0) break;
                                total += static_cast<size_t>(got);
                            }
                            state->buffer.resize(total);
                        } else {
                            std::ostringstream oss;
                            oss << body.rdbuf();
                            const auto& str = oss.str();
                            state->buffer.assign(str.begin(), str.end());
                        }
                    } else {
                        auto err = outcome.GetError();
                        state->error_message = err.GetMessage();
                        auto code = err.GetErrorType();
                        if (code == Aws::S3::S3Errors::NO_SUCH_KEY ||
                            code == Aws::S3::S3Errors::RESOURCE_NOT_FOUND)
                            state->error_http_code = 404;
                        else if (code == Aws::S3::S3Errors::ACCESS_DENIED ||
                                 code == Aws::S3::S3Errors::INVALID_ACCESS_KEY_ID ||
                                 code == Aws::S3::S3Errors::SIGNATURE_DOES_NOT_MATCH)
                            state->error_http_code = 502;
                    }

                    if (loop) loop->queueInLoop([h]() mutable { h.resume(); });
                    else      h.resume();
                });
        }

        std::vector<uint8_t> await_resume() {
            if (!state->error_message.empty())
                throw HttpException(state->error_http_code, state->error_message);
            return std::move(state->buffer);
        }
    };

    co_return co_await S3Awaiter{client_.get(), bucket, key, loop, state};
}
