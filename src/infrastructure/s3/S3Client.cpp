#include "infrastructure/s3/S3Client.hpp"
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/utils/threading/PooledThreadExecutor.h>
#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/S3Errors.h>
#include <trantor/net/EventLoop.h>
#include <memory>
#include <sstream>
#include <vector>

S3ClientWrapper::S3ClientWrapper(const AppConfig& cfg)
    : max_object_size_bytes_(cfg.max_object_size_bytes)
{
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

void S3ClientWrapper::shutdown() {
    // Dropping the shared_ptr<S3Client> transitively destroys the
    // PooledThreadExecutor we injected via ClientConfiguration; that
    // destructor joins its worker threads, so any currently-running
    // GetObjectAsync callback finishes before we return.
    client_.reset();
}

drogon::Task<std::vector<uint8_t>> S3ClientWrapper::download(
    const std::string& bucket, const std::string& key)
{
    auto* loop                    = trantor::EventLoop::getEventLoopOfCurrentThread();
    const long long max_bytes     = max_object_size_bytes_;

    struct S3State {
        std::vector<uint8_t> buffer;
        std::string          error_message;
        int                  error_http_code = 500;
    };
    // CancelGuard serialises coroutine-frame teardown against the
    // async callback's resume path. The awaiter's destructor flips
    // alive=false under the mutex; the callback checks it under the
    // same mutex before touching the coroutine handle. If Drogon
    // tears down the Task mid-flight (shutdown, or future request
    // cancellation), the callback bails instead of resuming a freed
    // frame (fixes C1).
    struct CancelGuard {
        std::mutex m;
        bool       alive = true;
    };
    auto state = std::make_shared<S3State>();
    auto guard = std::make_shared<CancelGuard>();

    struct S3Awaiter {
        Aws::S3::S3Client*           client;
        std::string                  bucket;
        std::string                  key;
        trantor::EventLoop*          loop;
        std::shared_ptr<S3State>     state;
        std::shared_ptr<CancelGuard> guard;
        long long                    max_bytes;   // 0 = unlimited

        ~S3Awaiter() {
            std::lock_guard lk(guard->m);
            guard->alive = false;
        }

        bool await_ready() { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            Aws::S3::Model::GetObjectRequest req;
            req.SetBucket(bucket);
            req.SetKey(key);

            client->GetObjectAsync(req,
                [state = state, guard = guard, loop = loop, max_bytes = max_bytes, h](
                    const Aws::S3::S3Client*,
                    const Aws::S3::Model::GetObjectRequest&,
                    Aws::S3::Model::GetObjectOutcome&& outcome,
                    const std::shared_ptr<const Aws::Client::AsyncCallerContext>&)
                {
                    if (outcome.IsSuccess()) {
                        auto& result = outcome.GetResult();
                        auto& body   = result.GetBody();
                        const long long len = result.GetContentLength();
                        // H1: reject oversized objects BEFORE vector::resize —
                        // otherwise a hostile/misconfigured endpoint advertising
                        // a huge Content-Length would force a multi-GB alloc.
                        if (max_bytes > 0 && len > max_bytes) {
                            state->error_http_code = 413;
                            state->error_message   =
                                "source object exceeds max_object_size_bytes";
                        } else if (len > 0) {
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
                            // No Content-Length advertised — read in chunks and
                            // enforce the same size cap as the length-known path.
                            constexpr size_t kChunk = 64 * 1024;
                            std::vector<char> chunk(kChunk);
                            while (body) {
                                body.read(chunk.data(),
                                          static_cast<std::streamsize>(kChunk));
                                const auto got = body.gcount();
                                if (got <= 0) break;
                                if (max_bytes > 0 &&
                                    state->buffer.size() + static_cast<size_t>(got)
                                        > static_cast<size_t>(max_bytes))
                                {
                                    state->error_http_code = 413;
                                    state->error_message   =
                                        "source object exceeds max_object_size_bytes";
                                    state->buffer.clear();
                                    break;
                                }
                                state->buffer.insert(state->buffer.end(),
                                    chunk.data(), chunk.data() + got);
                            }
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

                    auto resume_if_alive = [guard, h]() mutable {
                        std::unique_lock lk(guard->m);
                        if (!guard->alive) return;
                        lk.unlock();   // never call h.resume() under the lock:
                                       // the resumed coroutine's eventual
                                       // frame destruction re-acquires it.
                        h.resume();
                    };
                    if (loop) loop->queueInLoop(std::move(resume_if_alive));
                    else      resume_if_alive();
                });
        }

        std::vector<uint8_t> await_resume() {
            if (!state->error_message.empty())
                throw HttpException(state->error_http_code, state->error_message);
            return std::move(state->buffer);
        }
    };

    co_return co_await S3Awaiter{client_.get(), bucket, key, loop, state, guard, max_bytes};
}

drogon::Task<void> S3ClientWrapper::upload(
    const std::string& bucket, const std::string& key,
    std::vector<uint8_t> data, const std::string& content_type)
{
    auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();

    struct PutState {
        std::vector<uint8_t> data;   // pinned until the async call completes
        std::string          error_message;
        int                  error_http_code = 500;
    };
    struct CancelGuard {
        std::mutex m;
        bool       alive = true;
    };
    auto state = std::make_shared<PutState>();
    state->data = std::move(data);
    auto guard = std::make_shared<CancelGuard>();

    struct PutAwaiter {
        Aws::S3::S3Client*           client;
        std::string                  bucket;
        std::string                  key;
        std::string                  content_type;
        trantor::EventLoop*          loop;
        std::shared_ptr<PutState>    state;
        std::shared_ptr<CancelGuard> guard;

        ~PutAwaiter() {
            std::lock_guard lk(guard->m);
            guard->alive = false;
        }

        bool await_ready() { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            Aws::S3::Model::PutObjectRequest req;
            req.SetBucket(bucket);
            req.SetKey(key);
            if (!content_type.empty()) req.SetContentType(content_type);

            // PreallocatedStreamBuf wraps our vector bytes without copy.
            // The SDK drains the stream before the callback fires, so pinning
            // `state->data` through the callback keeps the buffer alive.
            auto streambuf = Aws::MakeShared<Aws::Utils::Stream::PreallocatedStreamBuf>(
                "imago-upload",
                state->data.data(),
                state->data.size());
            auto body = Aws::MakeShared<Aws::IOStream>("imago-upload", streambuf.get());
            req.SetBody(body);

            client->PutObjectAsync(req,
                [state = state, guard = guard, loop = loop, h](
                    const Aws::S3::S3Client*,
                    const Aws::S3::Model::PutObjectRequest&,
                    const Aws::S3::Model::PutObjectOutcome& outcome,
                    const std::shared_ptr<const Aws::Client::AsyncCallerContext>&)
                {
                    if (!outcome.IsSuccess()) {
                        const auto& err = outcome.GetError();
                        state->error_message = err.GetMessage();
                        auto code = err.GetErrorType();
                        if (code == Aws::S3::S3Errors::ACCESS_DENIED ||
                            code == Aws::S3::S3Errors::INVALID_ACCESS_KEY_ID ||
                            code == Aws::S3::S3Errors::SIGNATURE_DOES_NOT_MATCH)
                            state->error_http_code = 502;
                        else if (code == Aws::S3::S3Errors::NO_SUCH_BUCKET)
                            state->error_http_code = 404;
                    }

                    auto resume_if_alive = [guard, h]() mutable {
                        std::unique_lock lk(guard->m);
                        if (!guard->alive) return;
                        lk.unlock();
                        h.resume();
                    };
                    if (loop) loop->queueInLoop(std::move(resume_if_alive));
                    else      resume_if_alive();
                });
        }

        void await_resume() {
            if (!state->error_message.empty())
                throw HttpException(state->error_http_code, state->error_message);
        }
    };

    co_await PutAwaiter{client_.get(), bucket, key, content_type, loop, state, guard};
    co_return;
}
