# imgproxy-cpp: C++ Image Processing Service — Design Spec

**Date:** 2026-04-17  
**Phase:** 1 — Core image processing (resize, crop, convert) from S3  
**Goal:** Benchmark a C++23 image service against imgproxy under identical conditions

---

## 1. Tech Stack

| Component | Choice | Reason |
|---|---|---|
| Language | C++23 | Coroutine support (`co_await`), modern RAII |
| HTTP framework | Drogon | Async/coroutine-native, high performance |
| Image processing | libvips | Same library as imgproxy, fair comparison |
| S3 client | AWS SDK C++ | Official, supports MinIO via endpoint override |
| Build system | CMake + `CMakePresets.json` | All deps have official CMake support |
| Memory allocator | jemalloc (optional) | CMake flag `USE_JEMALLOC=ON/OFF`, default OFF |
| Sanitizers | AddressSanitizer | Debug build target, incompatible with jemalloc |
| Containerization | Docker + buildx | Multi-arch: `linux/amd64` + `linux/arm64` |

---

## 2. Source Code Structure

```
imgproxy-cpp/
├── CMakeLists.txt
├── CMakePresets.json               # presets: debug, release, asan
├── docker/
│   ├── Dockerfile                  # --build-arg BUILD_TYPE=Release|Debug
│   └── docker-compose.yml
├── benchmark/
│   └── run.sh                      # wrk/wrk2 scripts (provided separately)
├── asan/
│   └── suppressions.supp           # ASan suppressions for glib/vips/AWS false positives
├── include/
│   ├── config/
│   │   └── AppConfig.hpp           # env-var config loader
│   ├── controllers/
│   │   └── ImageController.hpp
│   ├── usecases/
│   │   └── ImageUseCase.hpp
│   ├── infrastructure/
│   │   ├── s3/
│   │   │   └── S3Client.hpp
│   │   └── vips/
│   │       ├── VipsImage.hpp       # move-only RAII wrapper for VipsImage*
│   │       └── ImageProcessor.hpp  # runs on libvips thread pool
│   └── models/
│       └── ImageRequest.hpp        # parsed query params struct
└── src/
    ├── main.cpp
    ├── controllers/
    │   └── ImageController.cpp
    ├── usecases/
    │   └── ImageUseCase.cpp
    └── infrastructure/
        ├── s3/
        │   └── S3Client.cpp
        └── vips/
            ├── VipsImage.cpp
            └── ImageProcessor.cpp
```

---

## 3. API Design

### URL Format

```
GET /{bucket}/{s3_key...}?method=resize|crop|convert&w=800&h=600&fit=fit|fill|fill-down|force|auto&output=webp|jpeg|png|avif
```

- `{bucket}` — S3 bucket name
- `{s3_key...}` — full S3 key, may contain slashes
- All processing options are query parameters

### Query Parameters

| Param | Values | Default | Required |
|---|---|---|---|
| `method` | `resize`, `crop`, `convert` | — | yes |
| `w` | integer px | `0` (auto) | no |
| `h` | integer px | `0` (auto) | no |
| `fit` | `fit`, `fill`, `fill-down`, `force`, `auto` | `fit` | no |
| `output` | `webp`, `jpeg`, `png`, `avif` | `webp` | no |

### Error Responses

| Condition | HTTP Status |
|---|---|
| Invalid / missing `method` | 400 |
| Invalid param values | 400 |
| S3 key not found | 404 |
| S3 auth failure | 502 |
| libvips processing error | 500 |

---

## 4. Data Flow

```
HTTP Request
  → ImageController
      parse {bucket}, {s3_key}, query params → ImageRequest
      validate params → 400 on failure
  → ImageUseCase::execute(ImageRequest) [coroutine]
      → co_await S3Client::download(bucket, key)
          returns std::vector<uint8_t>
      → co_await thread_pool:
          ImageProcessor::process(buffer, request)
              → VipsImage::load(buffer)
              → VipsImage::resize() | crop() | (load only for convert)
              → VipsImage::save(output_format)
              returns std::vector<uint8_t>
      → HttpResponse(200, image bytes, content-type)
```

### Concurrency Model

- **Drogon event loop threads** — handle HTTP I/O, coroutine scheduling
- **libvips thread pool** — dedicated fixed-size pool for all libvips operations
  - Size configured via `THREAD_POOL_SIZE` env var
  - libvips thread-local state is safe because work never migrates between pool threads
- **Coroutines own only I/O** — never hold libvips handles across `co_await` suspension points
- S3 download is async (coroutine); libvips work is posted to thread pool and `co_await`-ed

---

## 5. Memory Management Rules

### libvips (`VipsImage` wrapper)

- `VipsImage` wrapper holds raw `VipsImage*`, calls `g_object_unref()` in destructor
- Move-only: copy constructor and copy assignment are `= delete`
- All intermediate vips operations produce a new `VipsImage*` — assign to a new wrapper immediately
- Never hold a raw `VipsImage*` across a coroutine suspension point — move into thread pool closure
- Enable `vips_leak_set(TRUE)` in Debug builds

### AWS SDK C++

- `Aws::InitAPI` / `Aws::ShutdownAPI` called once in `main()`, never in request path
- `S3Client` is a `std::shared_ptr` singleton — thread-safe by AWS SDK design
- `GetObjectResult` IOStream read fully into `std::vector<uint8_t>` immediately; result goes out of scope before any `co_await`

### General Rules

- No `new`/`delete` outside RAII wrapper constructors/destructors
- All image byte buffers are `std::vector<uint8_t>` — no manual `malloc`/`free`
- jemalloc (`USE_JEMALLOC=ON`) is **force-disabled** when `BUILD_TYPE=Debug` (ASan incompatibility)

---

## 6. CMake Build System

### CMakePresets.json presets

| Preset | `BUILD_TYPE` | `USE_JEMALLOC` | ASan |
|---|---|---|---|
| `release` | Release | OFF (default) | no |
| `release-jemalloc` | Release | ON | no |
| `debug` | Debug | OFF (forced) | no |
| `asan` | Debug | OFF (forced) | yes (`-fsanitize=address,leak`) |

### Usage

```bash
cmake --preset asan
cmake --build --preset asan
```

### ASan flags (asan preset)

```cmake
target_compile_options(... -fsanitize=address,leak -fno-omit-frame-pointer)
target_link_options(... -fsanitize=address,leak)
```

---

## 7. Docker & Multi-Arch

### Dockerfile

```dockerfile
ARG BUILD_TYPE=Release
# build stage: cmake --preset based on BUILD_TYPE
# runtime stage: minimal image with libvips runtime libs
```

- Built with `docker buildx` targeting `linux/amd64` and `linux/arm64`
- Apple Silicon dev machines build `arm64` locally
- CI produces both via `--platform linux/amd64,linux/arm64`

### docker-compose.yml services

```yaml
services:
  imgproxy-cpp:
    build:
      args:
        BUILD_TYPE: Release
    cpus: "4"
    mem_limit: 4g
    environment:
      - THREAD_POOL_SIZE=4
      - DROGON_WORKERS=4
      - AWS_ACCESS_KEY_ID=...
      - AWS_SECRET_ACCESS_KEY=...
      - AWS_REGION=us-east-1
      - S3_ENDPOINT_URL=http://minio:9000
      - S3_FORCE_PATH_STYLE=true

  imgproxy-cpp-debug:
    build:
      args:
        BUILD_TYPE: Debug
    environment:
      - ASAN_OPTIONS=detect_leaks=1:suppressions=/app/asan/suppressions.supp:log_path=/tmp/asan.log
    volumes:
      - ./asan-output:/tmp

  imgproxy:
    image: darthsim/imgproxy
    cpus: "4"
    mem_limit: 4g
    environment:
      - IMGPROXY_MAX_CLIENTS=100
      - IMGPROXY_CONCURRENCY=4
      - IMGPROXY_CACHE_SIZE=0
      - IMGPROXY_SO_REUSEPORT=true

  minio:
    image: minio/minio
    command: server /data
    environment:
      - MINIO_ROOT_USER=minioadmin
      - MINIO_ROOT_PASSWORD=minioadmin
```

---

## 8. S3 / MinIO Configuration

| Env Var | Description | Default |
|---|---|---|
| `AWS_ACCESS_KEY_ID` | S3 / MinIO access key | required |
| `AWS_SECRET_ACCESS_KEY` | S3 / MinIO secret key | required |
| `AWS_REGION` | AWS region | `us-east-1` |
| `S3_ENDPOINT_URL` | Override endpoint (MinIO/localstack). Empty = real AWS | empty |
| `S3_FORCE_PATH_STYLE` | Required for MinIO path-style addressing | `false` |
| `THREAD_POOL_SIZE` | libvips worker thread count | `4` |
| `DROGON_WORKERS` | Drogon event loop thread count | `4` |

---

## 9. Benchmarking Plan

### Setup

- Docker Compose on a single machine, **one service at a time** (no resource contention)
- MinIO as S3 source — same bucket/keys for both services
- All caching disabled on both sides
- Benchmark scripts provided separately (`benchmark/run.sh`)

### Resource Constraints (both services identical)

- CPUs: 4 cores
- Memory: 4 GB

### Test Matrix

| Test | Image | Operation | Output | Concurrency |
|---|---|---|---|---|
| B1 | 4MP JPEG | resize 800×600 `fit` | webp | 10 / 50 / 100 |
| B2 | 4MP JPEG | resize 800×600 `fill` | webp | 10 / 50 / 100 |
| B3 | 4MP JPEG | crop 800×600 | webp | 10 / 50 / 100 |
| B4 | 4MP JPEG | convert only | webp | 10 / 50 / 100 |
| B5 | 10MP JPEG | resize 1920×1080 `fill` | webp | 50 |

### Metrics

- p50 / p95 / p99 latency
- Requests per second (RPS)
- Peak memory RSS
- CPU utilization %

### jemalloc Benchmark Pass

After baseline (system malloc) benchmark, rebuild with `--preset release-jemalloc` and repeat B1–B5 to measure allocator impact independently.
