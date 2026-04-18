# Solution Proposal: Image Processing Service

**Audience:** CTO  
**Author:** Engineering Team  
**Date:** 2026-04-15  
**Status:** Draft — Part 1 of 3

---

## 1. Problem Statement

### 1.1 The Role of Image Processing in Chat and Social Networks

In a chat or social network application targeting mobile devices, images are the primary medium of communication. Users send photos, receive profile avatars, browse media feeds, and share story content — all in a context where network speed, device memory, and battery life are constrained.

Serving images in their original form is not viable:

- A smartphone camera produces a 4–10 MB JPEG at 4000×3000 pixels. Sending that directly to a feed thumbnail slot of 160×160 would waste 99% of the bandwidth and force the client to decode and downsample on device — consuming CPU and battery.
- Social feed scrolling requires tens of images per second. Without server-side optimization, a slow 4G connection renders the feed unusable.
- Profile avatars, story previews, and reaction thumbnails each require different sizes and aspect ratios — the same source image must be presented in 5–10 distinct configurations across different UI contexts.
- WebP delivers 25–35% smaller file sizes than JPEG at equivalent perceptual quality. Without automatic format negotiation, mobile apps on older OS versions cannot benefit from modern codecs without manual fallback logic in the client.
- Watermarking, blur (for spoilers, NSFW flagging), and overlay compositing are core product features that must be applied at serve time, not baked into uploaded originals.

Without a dedicated image processing layer, the cost and user experience impact falls onto the client — the worst possible place for compute, memory, and battery.

### 1.2 Cost and Performance Impact

A well-designed image service has measurable impact on three dimensions:

**Bandwidth cost:**  
CDN egress is typically the largest infrastructure line item for media-heavy applications. Serving a correctly sized WebP instead of a full JPEG reduces per-image data transfer by 60–80%. At 10 million image requests per day, a 70% reduction in average payload size can eliminate hundreds of dollars per day in CDN egress charges.

**Server infrastructure cost:**  
An image processing service runs on CPU and memory. An inefficient service (high memory footprint per request, blocking I/O, or thread contention) requires more instances to handle peak load. The difference between a well-tuned service and a naive implementation is typically 3–10× in required instance count for the same throughput — directly proportional to cloud compute cost.

**User experience and retention:**  
Image load time is one of the top three contributors to feed engagement drop-off on mobile. A p99 latency of 5 seconds (as seen in unoptimized open-source solutions) is user-visible as blank image boxes. Dropping p99 from 5s to 200ms translates directly to higher retention metrics.

**Developer velocity:**  
Without a centralized image API, each client team (iOS, Android, Web) implements its own resize-and-cache logic. A shared image service with a simple HTTP API eliminates this duplication, standardizes quality settings, and lets product teams ship image-related features without client releases.

---

## 2. Essential Features for an Image Service

The following features form the minimum viable and recommended full feature set for a production image service serving a chat and social network product.

### 2.1 Image Upload and Ingestion

Before any transform can occur, the image must enter the system safely. Upload is not just file reception — it is the first and most important quality and security gate in the pipeline.

| Feature | Description | Priority |
|---------|-------------|----------|
| **EXIF stripping on ingest** | All user-uploaded images must have EXIF, IPTC, and XMP metadata stripped at upload time — not at serve time. EXIF can contain GPS coordinates, device serial numbers, and camera fingerprints. Stripping at ingest ensures the raw stored file never leaks sensitive metadata regardless of how it is later served. | Critical |
| **Pre-resize on ingest** | Images uploaded from mobile devices can be 4000×3000 px at 8–12 MB. Storing and re-processing originals at that resolution on every serve request wastes storage and compute. The upload endpoint should optionally pre-resize to a configurable maximum dimension (e.g., 2048px on the longest edge) before storage — preserving quality for all realistic display sizes while capping storage cost. | High |
| **Format normalization on ingest** | HEIC/HEIF images (default format on iOS cameras) are not universally supported by browsers or downstream services. The upload endpoint should optionally transcode HEIC → JPEG or WebP at ingest time, so all stored originals are in a universally readable format. | High |
| **Magic byte validation** | Before processing, validate the file's actual format via magic bytes — not the `Content-Type` header or filename extension, both of which can be forged. Reject files that do not match a supported image format. This prevents image bomb attacks and polyglot file exploits. | Critical |
| **Input size and pixel cap** | Reject uploads that exceed a configurable byte size (e.g., 20 MB) or pixel count (e.g., 50 MP) before decoding. Decoding an oversized image is the attack surface for image bomb DoS. | Critical |
| **Multipart and binary body support** | Support both `multipart/form-data` (standard browser form upload) and raw binary POST body (efficient mobile SDK upload). The source selection should be transparent to the processing pipeline. | Critical |

**Design note:** EXIF stripping and pre-resize should happen in the upload path, not the serve path. Applying them at serve time means the raw stored file always contains the original sensitive metadata, and every unique serve configuration triggers a full re-process of the original large image. Doing it at ingest — once, at write time — keeps stored files clean, small, and safe from the moment they are persisted.

---

### 2.2 Core Transform Operations

| Feature | Description | Priority |
|---------|-------------|----------|
| **Resize** | Scale image to target width, height, or bounding box while preserving or forcing aspect ratio. Must support `fit` (letterbox), `fill` (cover-crop), and `force` modes. | Critical |
| **Crop** | Extract a region from the image. Supports gravity-based anchor (top, bottom, centre, left, right) and explicit pixel offset. | Critical |
| **Smart Crop** | Content-aware crop using libvips `vips_smartcrop` — automatically selects the most visually important region. Essential for avatar generation from full-body photos or landscape shots. | High |
| **Convert** | Transcode between JPEG, WebP, PNG, AVIF. Must support auto-negotiation from the `Accept` header — serve WebP to browsers/clients that support it, JPEG as fallback. | Critical |
| **Watermark** | Composite a text or image watermark onto the output. Configurable position (gravity), opacity, tiling, and scale. Required for copyright protection on shared media and brand overlays. | High |
| **Blur** | Gaussian blur with configurable sigma. Used for NSFW content warnings, spoiler images, background defocus in stories, and privacy masking. | High |
| **Embed / Letterbox** | Resize an image to fit within a fixed canvas, filling the remaining area with a solid color, mirror extension, or pixel copy. Required for feed grids where uniform tile size is enforced. | High |
| **Enlarge / Upscale** | Optionally allow upscaling when the source is smaller than the target. Can be disabled (passthrough) to avoid quality loss. | Medium |
| **Format Quality Control** | Per-format quality settings (JPEG quality 1–100, WebP quality, PNG compression level). Configurable defaults and per-request override. | Critical |

### 2.3 Metadata and Introspection

| Feature | Description | Priority |
|---------|-------------|----------|
| **Info / Metadata** | Return image dimensions, format, channel count, and EXIF data as JSON without performing any transformation. Used by clients to build responsive layouts before downloading the full image. | High |
| **EXIF Auto-Rotate** | Automatically apply EXIF orientation flags so all output is correctly oriented regardless of capture device. Without this, portrait photos appear rotated on some clients. | Critical |
| **Strip Metadata** | Remove EXIF, IPTC, and XMP metadata from output. Reduces file size and prevents leaking GPS coordinates or device fingerprints from user-uploaded photos. | Critical |

### 2.4 Advanced and Recommended Features

| Feature | Description | Priority |
|---------|-------------|----------|
| **Pipeline** | Chain multiple operations in a single request (e.g., resize → watermark → convert). Eliminates round trips and intermediate storage for multi-step transforms. | High |
| **Rotate / Flip / Flop** | Manual rotation (90°/180°/270°), horizontal and vertical flip. Required for user-facing editing tools. | Medium |
| **Sharpen** | Unsharp-mask sharpening after downscale, recovers detail lost in aggressive thumbnail shrinking. | Medium |
| **Background Fill** | Specify background color for transparent PNG → JPEG conversion and letterbox padding. CSS named colors and hex accepted. | Medium |
| **Focal Point Crop** | Client-specified focal point (as X/Y fractions) for gravity-aware crop — lets the CMS or upload flow declare "the face is at 0.3, 0.2" so thumbnails never cut off the subject. | Medium |
| **DPR / Retina Support** | Scale target dimensions by device pixel ratio (1×, 2×, 3×). Allows a single URL pattern to serve both standard and retina displays. | Medium |
| **Conditional GET (ETag / 304)** | Generate a deterministic ETag from transform parameters + image content. Return 304 Not Modified when the client already has the current version. Reduces redundant transfers significantly for feeds with stable images. | High |
| **Image Source Abstraction** | Support multiple input sources: POST body (upload path), URL fetch (proxy path), S3/GCS/object storage (internal path). Selecting the right source per use case avoids double-upload architectures. | High |
| **Progressive / Interlaced Output** | Interlaced JPEG and PNG load progressively on slow connections — the user sees a blurry preview that sharpens as bytes arrive. Critical for large media on poor mobile connections. | Medium |
| **Lossless WebP** | Lossless WebP encoding for graphics, screenshots, and diagrams where pixel accuracy is required. | Low |
| **Color Adjustments** | Brightness, contrast, saturation, hue rotation. Required for filters in a story or editing feature. | Low |
| **Shape Mask** | Apply a shape mask (circle, rounded square) to output — used for avatar display in chat UIs without client-side clipping. | Low |
| **Placeholder on Error** | Return a configurable fallback image instead of an error response — prevents broken image icons in feeds when a source URL is temporarily unavailable. | Medium |

### 2.5 Operational Requirements

Beyond transform features, a production service requires:

- **HMAC URL signing** — prevents DoS attacks where anyone can generate arbitrary CPU-intensive transform URLs.
- **Input size and pixel limits** — rejects image bombs before libvips decodes them.
- **Rate limiting** — per-client or global, to protect against burst traffic.
- **Health check endpoint** — required for load balancer health probes and container orchestration readiness gates.
- **Prometheus / OpenTelemetry metrics** — RPS, latency histograms, error rates, memory consumption.
- **Configurable cache headers** — `Cache-Control`, `Vary: Accept`, `ETag` — to work correctly with CDN layers.
- **SSRF protection** — when proxying from a `?url=` parameter, block requests to internal network ranges.
- **Graceful shutdown** — drain in-flight requests before process termination to avoid mid-stream response corruption.

---

## 3. Market Solutions

Three open-source image processing services are commonly used in production. This section covers their architecture, feature set, advantages, disadvantages, and known problems in the context of a chat/social network deployment.

---

### 3.1 imgproxy

**Repository:** https://github.com/imgproxy/imgproxy  
**Language:** Go  
**Core engine:** libvips (via CGo)  
**License:** MIT (OSS) / Commercial (Pro)

#### Architecture Summary

imgproxy is a standalone stateless HTTP service. All transformation parameters are encoded into a signed URL path:

```
/{signature}/{processing_options}/plain/{source_url}@{extension}
```

The server fetches the source image, runs a 14-stage libvips pipeline, and streams the result. There is no internal image cache — caching is delegated entirely to CDN or nginx upstream.

Concurrency is controlled by two semaphores: a queue semaphore (immediate 429 if full) and a worker semaphore (blocks until a worker is free). Each worker goroutine pins to an OS thread and calls libvips with `VIPS_CONCURRENCY=1`, achieving parallelism at the goroutine level without libvips internal thread contention.

#### Feature Coverage

| Category | Features |
|----------|----------|
| Resize | fit, fill, fill-down, force, auto; width, height, zoom, DPR |
| Crop | gravity (centre, smart, focal point), manual offset |
| Format | JPEG, PNG, WebP, AVIF, GIF, JXL, TIFF, BMP, ICO; auto from Accept |
| Quality | per-format quality, adaptive quality (max-bytes retry loop) |
| Effects | blur, sharpen, pixelate, rotate, flip |
| Watermark | image watermark (scale, gravity, opacity, tile) |
| Extend | pad with color/mirror/copy |
| Pipeline | single transform per request (no multi-step pipeline) |
| Info | `/info/` endpoint — dimensions, format, EXIF |
| ETag | composite ETag over processing options + origin ETag or image hash |
| Sources | HTTP URL, S3, GCS, Azure Blob, local FS, Swift |
| Security | HMAC-SHA256 signing, source allowlist, SSRF block, magic byte check, SVG sanitize |
| Observability | Prometheus, Datadog, OpenTelemetry, New Relic, CloudWatch |
| Animation | GIF/WebP/AVIF animation support (frame cap configurable) |

#### Advantages

- **Production-grade security:** HMAC URL signing with key rotation support is built-in and mandatory. No URL can be generated without the signing key — DoS via arbitrary URL generation is structurally prevented.
- **Excellent observability:** Prometheus, OpenTelemetry, Datadog, New Relic all ship out of the box. Metrics include libvips memory tracking, buffer pool calibration, and worker concurrency.
- **S3/GCS/ABS pluggable transports:** Direct object storage integration means no intermediate HTTP server is needed for internally stored images.
- **Adaptive quality compression:** The `max_bytes` feature retries encoding with progressively lower quality until the output fits under a size cap — important for messaging where file size limits apply.
- **Scale-on-load optimization:** Per-format shrink-on-load (JPEG 1/2/4/8, WebP fractional) is implemented explicitly — the single biggest throughput win for thumbnail workloads.
- **AVIF and JXL support:** Future-proof format support for next-generation compression.
- **Preset system:** Named processing presets reduce URL complexity and prevent unauthorized option combinations.

#### Disadvantages

- **High CPU usage:** Because libvips's internal thread pool is set to 1 per goroutine and GOMAXPROCS goroutines run concurrently, imgproxy saturates all CPU cores during image-heavy traffic. In our benchmark, JPEG w=512 resize consumed **666% CPU** for 266 RPS — 5500× more CPU per request than an equivalent C++ implementation.
- **Memory footprint moderate but not minimal:** ~330–680 MB range in benchmarks. Not problematic on modern instances, but relevant for high-density container deployments.
- **CGo overhead:** Every libvips call crosses the Go↔C boundary, with `runtime.LockOSThread()` per worker. This is measurable in profiling, though not a dominant cost.
- **No multi-step pipeline per request:** Each request is a single transform. Composing resize → watermark → convert requires chaining requests or building a pipeline layer above imgproxy.
- **No in-process result cache:** Each cache miss triggers a full re-process. Without a CDN or nginx proxy_cache upstream, repeated identical requests are fully re-processed.
- **Commercial feature gate:** Smart crop, AVIF, JXL, presets, and some security features require the Pro license for production use. The OSS version is feature-restricted.

#### Known Problems

- **Tail latency on edge cases:** Some operations (embed with certain gravity modes, convert to certain formats) show p99 near 5 seconds in our benchmarks — indicative of GC pressure or lock contention under load.
- **libvips op cache disabled by default:** Required to avoid a SIGSEGV on musl libc. This means every request re-initializes libvips operations from scratch — correct behavior but worth understanding when profiling.
- **goroutine leak risk under CGo:** If a libvips call panics without proper recovery, the goroutine holding the OS thread lock can leak. imgproxy includes `defer/recover` but it relies on Go's panic semantics surviving CGo → possible stability concerns under malformed input.

---

### 3.2 imaginary

**Repository:** https://github.com/h2non/imaginary  
**Language:** Go  
**Core engine:** libvips (via `bimg` wrapper library)  
**License:** MIT  
**Status:** Minimally maintained (last major release 2023)

#### Architecture Summary

imaginary is a flat, minimal Go HTTP service using only the standard `net/http` library. Each image operation has its own endpoint (`/resize`, `/crop`, `/watermark`, etc.). All operations call through `bimg` (a Go libvips wrapper) which ultimately calls `bimg.Resize()` for all transforms. There is no internal cache; all caching is via HTTP headers.

Concurrency uses Go's goroutine-per-request model with an optional GCRA rate limiter. No worker semaphore — libvips is called directly from request goroutines with libvips's default concurrency settings (multi-threaded internally).

#### Feature Coverage

| Category | Features |
|----------|----------|
| Resize | fit, fill, enlarge, thumbnail, zoom |
| Crop | gravity-based, smart crop (`/smartcrop`) |
| Format | JPEG, PNG, WebP, TIFF, GIF, SVG (pass-through), PDF (pass-through) |
| Quality | JPEG quality, PNG compression level |
| Effects | Gaussian blur, sharpen |
| Watermark | text watermark, image watermark (remote URL) |
| Rotate | manual (90° multiples), auto-rotate (EXIF), flip, flop |
| Extract | arbitrary region at pixel offset |
| Pipeline | `/pipeline` endpoint — up to 10 sequential operations in one request |
| Info | `/info` endpoint — dimensions, format, channels, alpha |
| Sources | POST binary body, POST multipart, GET ?url= (URL fetch), GET ?file= (local FS) |
| Security | HMAC-SHA256 URL signing (query-param based), single global API key |
| Observability | `/health` endpoint only (Go runtime memory stats) |
| Placeholder | configurable fallback image on error |

#### Advantages

- **Simple, flat API:** One endpoint per operation makes integration straightforward. No URL encoding complexity. REST-friendly — easy to call from any HTTP client.
- **Pipeline endpoint:** `/pipeline` with a JSON operations array is the cleanest multi-step API of the three solutions. Up to 10 operations can be chained with `ignore_failure` per step.
- **Smart crop built-in (OSS):** `/smartcrop` is available without a commercial license.
- **Placeholder on error:** Returns a fallback image instead of a JSON error — prevents broken image tags in feeds.
- **Minimal dependencies:** 4 Go libraries total. Straightforward to understand, audit, and fork.
- **Local FS and URL sources:** Both POST upload and URL proxy supported without configuration flags in the Dockerfile.
- **jemalloc in Docker:** Preconfigured to reduce C-heap fragmentation under libvips workloads.

#### Disadvantages

- **Minimal observability:** Only a `/health` endpoint with Go runtime stats. No Prometheus metrics, no distributed tracing, no per-endpoint latency histograms. Operating imaginary in production without external APM is largely blind.
- **No S3/GCS source:** No built-in object storage transport. Images stored in S3 must be pre-signed and fetched via the URL source — adding latency and a network hop.
- **Maintenance status:** The project is minimally maintained. bimg (the underlying libvips binding) is also inactive. Known issues with modern libvips versions have open PRs that are not being merged. Running imaginary on libvips 8.15+ requires patch work.
- **No AVIF or JXL support:** bimg does not expose AVIF or JXL encoding. Format support is frozen at the bimg library's capability.
- **No ETag / conditional GET:** No built-in ETag generation. Clients cannot perform conditional GET requests — every fetch re-downloads the image body regardless of whether it changed.
- **Single global API key:** Auth is a single shared secret. No per-client keys, no JWT, no signing key rotation.
- **libvips concurrency not explicitly managed:** imaginary does not set `VIPS_CONCURRENCY=1` per goroutine. Each goroutine may start multi-threaded libvips operations internally, leading to thread over-subscription on multi-core hosts.
- **No URL signing on POST:** URL signing protects GET requests only. POST operations are unprotected by signing.

#### Known Problems

- **bimg wraps everything through `bimg.Resize()`:** Even a simple format conversion or rotation goes through the full resize code path. This creates subtle parameter interaction bugs and makes it impossible to optimize individual operations independently.
- **Encode fallback silently changes format:** If WebP or HEIF encoding fails, imaginary silently falls back to JPEG without informing the client. Clients expecting WebP receive JPEG with no indication.
- **Memory grows without bound at high load:** No explicit libvips op cache disable (bimg doesn't expose this). Under sustained high load, libvips's op cache fills with unrepeated operations and memory grows. The `FreeOSMemory()` background goroutine (every 30s) is a band-aid, not a fix.
- **No graceful queue:** When traffic exceeds `--concurrency`, the GCRA limiter returns HTTP 429 immediately with no queue — all burst traffic is dropped rather than held briefly.
- **libvips version mismatch risk:** bimg pins specific libvips function signatures. A libvips upgrade in the base OS image can silently break operations.

---

### 3.3 weserv/images

**Repository:** https://github.com/weserv/images  
**Language:** C++17  
**Core engine:** libvips (direct C++ API)  
**License:** BSD-3-Clause

#### Architecture Summary

weserv/images is an **nginx content module**, not a standalone service. It compiles directly into nginx and runs as a content-phase handler inside nginx worker processes. Requests arrive at nginx; the module parses query params, issues an nginx upstream subrequest to fetch the source image, processes it synchronously with libvips, and streams the result through nginx buffer chains.

There is no separate process — nginx is both the HTTP server and the image processor. nginx's built-in `proxy_cache` with tmpfs (`/dev/shm`) provides an L1 result cache for repeated requests.

Each nginx worker process is single-threaded. During libvips processing, the entire worker's event loop is blocked. Effective concurrency = number of nginx worker processes (typically one per CPU core).

#### Feature Coverage

| Category | Features |
|----------|----------|
| Resize | contain, inside, outside, cover, fill, crop; DPR |
| Crop | gravity (top/bottom/left/right/centre/entropy/attention), focal point, manual offset; pre-crop |
| Smart Crop | `a=attention` or `a=entropy` — libvips content-aware positioning |
| Format | JPEG, PNG, WebP, AVIF, GIF, JXL, TIFF; auto-format via Accept |
| Quality | per-format quality, lossless WebP/AVIF, PNG compression level |
| Effects | Gaussian blur, unsharp-mask sharpen, contrast, gamma |
| Color | saturation, hue rotation, brightness, tint |
| Background | background color for alpha flatten, canvas, rotation fill |
| Mask | SVG path shapes: circle, ellipse, triangle, heart, hexagon, star, etc. |
| Rotate | arbitrary angle (non-90° fills background), flip, flop |
| Trim | border trim with threshold detection |
| Animation | multi-frame GIF/WebP/AVIF with per-frame delay and loop control |
| Info | `output=json` — dimensions, format, EXIF |
| Sources | URL fetch only (`?url=` required); no POST body, no S3/GCS |
| Caching | nginx `proxy_cache` on tmpfs — processed image cache |
| Rate Limiting | nginx `limit_req_zone` or distributed Valkey GCRA module |
| Security | IP blocklist, pixel limit, size limit, process timeout; no URL signing |
| Observability | nginx access/error logs only; no Prometheus |

#### Advantages

- **Direct libvips C++ API:** No CGo, no FFI, no Go GC. libvips calls are zero-overhead native C++ method chains. This is the most direct possible integration with libvips.
- **nginx integration gives features for free:** TLS termination, HTTP/2, worker process isolation, connection limits, access logging, and `proxy_cache` are all nginx built-ins — no application code needed.
- **Built-in L1 result cache:** `proxy_cache` on tmpfs caches processed images in RAM. A repeated request for the same URL is served from cache without re-running libvips. No other solution in this comparison includes a result cache.
- **Process isolation:** Each nginx worker is a separate OS process. A memory leak or GLib arena fragmentation in one worker does not affect others. Workers are restartable independently.
- **Rich feature set:** Shape masks, color adjustments, per-frame animation control, and arbitrary-angle rotation are available in the OSS version with no license restriction.
- **Widest format support:** JXL, AVIF, GIF animation, TIFF — all supported without commercial licensing.

#### Disadvantages

- **Synchronous blocking on nginx event loop:** libvips runs on the nginx worker thread, blocking the event loop for the duration of processing. This means under concurrent image-heavy load, each request fully occupies a CPU core and the event loop. There is no async/non-blocking processing model.
- **No standalone operation:** weserv cannot run without nginx. Containerizing it requires embedding the weserv module into an nginx build — a non-trivial Docker image with custom nginx compilation.
- **No POST body source:** weserv only accepts images via `?url=` — it cannot receive images as a POST body. For an upload-then-transform flow, images must be stored somewhere accessible by URL before weserv can process them. This adds architectural complexity (need a storage layer in the path).
- **No URL signing:** The API is entirely open. Any URL with any parameters is valid. In a self-hosted deployment without additional nginx auth (`allow`/`deny`, bearer token), the service is vulnerable to DoS via arbitrary CPU-intensive transform URLs.
- **Extremely high memory usage:** In our benchmarks, weserv consumed **1368–2834 MB** of RSS across all scenarios — 4–8× more than imago and 2–4× more than imgproxy. This appears to be a combination of nginx's buffer chain model, libvips operation memory, and large per-worker heap allocation.
- **Severe tail latency:** In the 2026-04-14 benchmark, many scenarios showed **p99 latency at or near 5 seconds** (the hey timeout limit). Many Embed, Auto-format, Crop, and Convert scenarios showed p99 ≥ 5s, meaning 1% of requests were not completing within 5 seconds under 20 concurrent clients with 300 total requests.
- **Minimal observability:** nginx access logs are the only built-in signal. No Prometheus, no OpenTelemetry, no per-endpoint latency tracking. External nginx-prometheus-exporter must be added separately.
- **Inconsistent throughput between runs:** Our benchmark data shows weserv results varying dramatically between runs (e.g., JPEG w=512: 265 RPS in one run, 28 RPS in another). This suggests sensitivity to nginx worker scheduling, memory pressure, or internal libvips threading interaction that is difficult to tune predictably.
- **No ETag / conditional GET:** No built-in ETag generation.
- **No health check endpoint:** No application-level health probe. Container orchestration must rely on TCP checks.

#### Known Problems

- **Memory footprint is not explained by feature set:** weserv uses 4–8× more memory than imgproxy for equivalent operations, despite both using libvips. The most likely cause is nginx's buffer chain model (nginx duplicates buffers at each filter stage) combined with weserv's `proxy_cache` warm-up and GLib arena growth per worker over time.
- **SSRF is partially unprotected:** The `weserv_deny_ip` directive only blocks exact IPs. Private subnets must be manually enumerated. There is no automatic SSRF protection for link-local addresses (169.254.x.x) beyond what is listed.
- **Nginx module compilation complexity:** Upgrading nginx or libvips independently requires recompiling the module from source. Binary compatibility between nginx module ABI versions is not guaranteed. This makes security patch application slower than a standalone binary.
- **No graceful shutdown semantics:** nginx `reload` (SIGHUP) forks a new master and drains the old workers, but the weserv module has no equivalent application-level drain hook. In-flight libvips operations are interrupted on worker termination.
- **Rate limiting requires external component:** The recommended distributed GCRA rate limiter requires a separate Valkey (Redis-compatible) instance. nginx's built-in `limit_req_zone` does not share state across processes or instances.

---

## Summary Comparison Table

| Dimension | imgproxy | imaginary | weserv/images |
|-----------|----------|-----------|---------------|
| Language | Go | Go | C++17 |
| Deployment model | Standalone binary | Standalone binary | nginx module |
| Image engine | libvips (CGo) | libvips (bimg CGo) | libvips (C++ direct) |
| Throughput (JPEG resize) | ~266 RPS | ~N/A | ~28–265 RPS (unstable) |
| CPU per request | Very high (666%+ CPU) | High | High (blocking event loop) |
| Memory footprint | Moderate (330–680 MB) | Moderate | Very high (1368–2834 MB) |
| p99 latency stability | Good for most ops | Moderate | Poor (many ops ≥ 5s p99) |
| URL signing (HMAC) | Yes (required) | Optional | No |
| Smart crop | Pro only | Yes (OSS) | Yes (`a=attention/entropy`) |
| AVIF / JXL | Yes | No | Yes |
| Pipeline (multi-op) | No | Yes (max 10) | No |
| ETag / 304 | Yes | No | No |
| S3 / GCS source | Yes | No | No |
| POST body source | No | Yes | No |
| Result cache | No (CDN delegation) | No | Yes (nginx proxy_cache) |
| Prometheus metrics | Yes | No | No |
| OpenTelemetry | Yes | No | No |
| Active maintenance | Yes | Minimal | Yes |
| Commercial license needed | Some features | No | No |
| Observability | Excellent | Minimal | Minimal |

---

---

## 4. Design Criteria

Criteria to evaluate any implementation — whether building in-house or adopting an open-source solution. These are the questions to answer before finalizing an architecture.

---

### 4.1 On-Demand Synchronous Processing

The service must return the processed image **in the same HTTP response** — no job queues, no polling, no callbacks.

```
Client sends request
      ↓
GET /resize?w=512&url=photo.jpg
      ↓
Service processes → responds with image bytes
      ↓
Client receives image  ✓
```

**Why this is non-negotiable:**

- An `<img src="...">` tag or a mobile `ImageView` cannot poll. The OS expects a single HTTP request that returns image bytes. An async pipeline requires a separate notify-then-reload flow in every client on every platform.
- A user scrolling a feed has ~300ms before a blank tile is noticeable. Async jobs add queue wait time, scheduling, and notification round trips on top of processing time — impossible to fit that window.
- On-demand fits CDN caching perfectly. The first request computes the result; every subsequent request for the same `(image + transform)` pair is served from cache in < 20ms. The service only ever sees cache misses.
- No persistent state — no job records to store, no status to poll, no completed jobs to clean up.

**What makes it viable at scale:**

1. **Processing must be fast** — resize must complete in under 300ms (p99). This drives the choice of image library, shrink-on-load optimization, and worker pool sizing.
2. **Cache absorbs repeat requests** — at 90% cache hit rate, the service handles only 10% of actual traffic volume. `Cache-Control` headers and ETag design are as important as the processing pipeline itself.

**The one exception — batch backfill:**  
Generating a new thumbnail size for millions of existing images is a background job, not a user-facing request. That is the only appropriate use of an async queue. All user-facing operations — upload and serve — must be synchronous.

---

### 4.2 Performance

**Latency targets (define before building)**

| Operation | Target (p99) |
|-----------|-------------|
| Upload (EXIF strip + pre-resize + store) | < 1s |
| Serve — cache miss (resize, crop, convert) | < 300ms |
| Serve — cache hit (CDN or nginx) | < 20ms |

**Throughput per instance**  
Define the target RPS per container before sizing the worker pool and queue depth. This number determines auto-scale thresholds and instance type selection.

**Shrink-on-load is mandatory**  
Decode and resize must happen in a single pass using the codec's native subsampling (JPEG 1/2/4/8, WebP fractional scale). Full-decode then resize is 4–8× slower and is not acceptable on the hot path.

---

### 4.3 Memory and Cost

**Memory per request, not just idle RSS**  
Peak memory during a single large image transform can be 3–5× the input file size (decode buffer + intermediate image + encode buffer). Under concurrent load this multiplies. Define an acceptable per-instance RSS ceiling.

**Cache hit rate is the primary cost lever**  
A 1% improvement in cache hit rate at 100M requests/day has more cost impact than any server-side optimization. Design `Cache-Control` headers and ETag strategy before designing the processing pipeline.

**Storage cost of originals**  
Pre-resizing at ingest (e.g., cap at 2048px longest edge) reduces storage cost proportionally. Define the max ingest dimension as a business decision, not an engineering default.

---

### 4.4 Reliability

**Backpressure must be explicit**  
When the service reaches capacity, upstream callers must receive a clear signal (HTTP 503 with `Retry-After`) rather than a timeout. Timeouts are silent and cause cascading failures. Define the queue depth and the shed-load threshold before deployment.

**Fallback on source unavailability**  
When the source image URL is unreachable, the options are: return an error, return a placeholder image, or return the last cached version. Each has different UX implications. Define the contract before implementation.

**Graceful shutdown**  
In-flight processing jobs must complete before the process exits. A container restart during a 200ms resize must not return a truncated response to the client.

**Input validation as a reliability gate**  
Malformed images, truncated files, and adversarial inputs (image bombs, polyglot files) must be rejected before reaching the image processing library. This is both a reliability and a security requirement.

---

### 4.5 Security

**HMAC URL signing is non-optional for a self-hosted service**  
Without signing, any user can construct an arbitrary transform URL (e.g., resize a 50 MP TIFF to 10 different sizes simultaneously) and exhaust server CPU at zero cost to them.

**SSRF protection on URL-fetch source**  
The `?url=` parameter must not be allowed to reach internal network addresses. A naive implementation becomes an internal network scanner.

**EXIF stripping is a privacy requirement, not a feature**  
GPS coordinates in chat photos is a data leak. Treat EXIF stripping as a security requirement. It must happen at ingest — the raw stored file must never contain location data.

**Define the authorization boundary**

| Endpoint | Access model |
|----------|-------------|
| Upload | Authenticated users only, per-user rate limiting |
| Serve | Public (CDN-cacheable), URL-signed to prevent arbitrary transforms |

---

### 4.6 Scalability

**Stateless by design**  
No in-process image cache, no sticky sessions. Every instance must be able to handle any request. The caching layer (CDN / nginx) is external and shared.

**Separate upload and serve scaling**  
Upload is write-heavy and CPU-bound (EXIF strip + resize + store). Serve is read-heavy and cache-friendly. They have different scaling profiles. Either deploy as separate services or use separate concurrency pools within one process.

---

### 4.7 Observability

**Minimum required metrics**

| Metric | Why |
|--------|-----|
| RPS + error rate per endpoint | Detect regressions |
| Latency histogram (p50/p95/p99) per operation | Catch tail latency before users do |
| Worker queue depth + utilization | Predict when to scale |
| Memory RSS over time | Detect slow leaks |
| Cache hit rate (CDN + local) | Primary cost signal |

**Structured log per request**  
Input dimensions → output dimensions, processing time, format in → format out, source type. Without this, debugging a slow request in production is guesswork.

**Health check that reflects actual readiness**  
Not just "TCP port open" but "image library is initialized and the worker pool is accepting jobs." A container can be running but unable to process images.

---

### 4.8 Operational Ergonomics

**Configuration via environment variables**  
Every tunable knob (worker count, quality defaults, size limits, signing key, cache TTL) must be settable via environment variable. Config files require volume mounts or image rebuilds in container deployments.

**Lean Docker image**  
libvips adds ~30–50 MB to the binary. A distroless or scratch-based image with a statically linked binary keeps the image under 100 MB and makes rollouts fast. A 2 GB Ubuntu-based image adds 30–60 seconds to every deploy cycle.

**The one number to agree on first**  
Before any design decision: **what is the p99 serve latency target for a cache miss?** Everything else — worker pool size, instance type, queue depth — flows from that number. 100ms, 300ms, and 1000ms lead to meaningfully different architectures.

---

*[End of Part 1 — Problem Statement, Essential Features, Market Solutions, and Design Criteria]*

---

## Part 2: Proposed Solution Design

---

## 5. System Architecture

### 5.1 Overview

The proposed solution is **imago** — a single C++23 binary combining image upload and image processing in one deployable service. It is deployed as two independent ECS services (same Docker image, different routing) to allow upload and serve traffic to scale independently.

**Stack:** C++23 · Drogon · libvips · AWS S3 SDK · jemalloc

**Key architectural properties:**
- Drogon IO threads never call libvips — all image processing runs on a dedicated `VipsWorkerPool`
- C++20 coroutines bridge IO and CPU threads: `co_await pool.submit(...)` suspends without blocking
- Fully stateless — no in-process cache, no session state, no database
- S3 is the single source of truth for all image data
- CloudFront absorbs all repeat requests — imago only sees cache misses

### 5.2 Architecture Diagram

> Full interactive diagram: [`docs/architecture-diagram.drawio`](./architecture-diagram.drawio)  
> Open with draw.io desktop app, VS Code draw.io extension, or app.diagrams.net.

### 5.3 Component Map

```
┌─────────────────────────────────────────────────────────────────┐
│  UPLOAD PATH                                                     │
│                                                                  │
│  Mobile Client                                                   │
│      │                                                           │
│      ├─ Path A: Direct to S3 ──────────────────────────────┐    │
│      │   ① POST /v1/upload/presign → imago-uploader        │    │
│      │   ② ← { presigned_url, s3_key }                     │    │
│      │   ③ PUT image → S3 directly  (no EXIF strip)        │    │
│      │   ④ POST /v1/upload/confirm → imago does S3 HEAD    │    │
│      │                                                      │    │
│      └─ Path B: Server upload ────────────────────────────┐│    │
│          ⑤ POST /v1/upload/binary → imago-uploader        ││    │
│             · Strip EXIF                                   ││    │
│             · Validate magic bytes                         ││    │
│          ⑥ PUT (EXIF stripped) → S3                       ││    │
│                                                            ││    │
│                    imago-uploader ─────────────────── S3 ◄┘┘    │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  SERVE PATH  —  On-Demand + CloudFront Cache                    │
│                                                                  │
│  Mobile Client                                                   │
│      │  GET /users/123/photo_v2.jpg?w=512&h=512&format=webp     │
│      ▼                                                           │
│  CloudFront                                                      │
│      │                                                           │
│      ├─ Cache HIT  ──────────────────────────────────────────   │
│      │   Response in < 20ms, imago never touched                │
│      │                                                           │
│      └─ Cache MISS                                               │
│          │  + X-Origin-Token header                              │
│          ▼                                                        │
│        ALB  ──►  imago-processor                                 │
│                      │  1. Validate X-Origin-Token               │
│                      │  2. Parse s3_key from path                │
│                      │  3. GetObject(key) from S3                │
│                      │  4. co_await vipsPool.submit(...)         │
│                      │     · shrink-on-load decode               │
│                      │     · resize / crop / convert             │
│                      │     · encode to requested format          │
│                      │  5. Return image + Cache-Control + ETag   │
│                      ▼                                           │
│                  CloudFront caches result                        │
│                  Client receives image < 300ms                   │
└─────────────────────────────────────────────────────────────────┘
```

### 5.4 Two ECS Services, One Binary

The same Docker image is deployed as two ECS services. ALB routes by path prefix:

| ECS Service | ALB Rule | Scales on |
|-------------|----------|-----------|
| `imago-uploader` | `/v1/upload/*` | CPU (EXIF strip + pre-resize is CPU-bound) |
| `imago-processor` | `/*` | CPU + RAM (libvips transform under load) |

This allows upload spikes (morning/evening chat activity) and serve spikes (feed loading) to scale independently without affecting each other.

### 5.5 Thread Model

```
Drogon IO Threads  (N threads — epoll, HTTP parse, S3 SDK async)
        │
        │  co_await vipsPool.submit(lambda)
        ▼
VipsWorkerPool  (M threads — all libvips runs here, VIPS_CONCURRENCY=1 per worker)
        │
        ▼
ImagePipeline  (shrink-on-load → resize/crop/convert → encode)
```

- `VIPS_CONCURRENCY=1` per worker — parallelism at thread level, no libvips internal thread contention
- libvips op cache disabled (`vips_cache_set_max(0)`) — diverse workload, cache never hits, wastes memory
- `vips_thread_shutdown()` called per worker after each job — prevents thread-local memory leaks
- jemalloc replaces glibc malloc — better arena management for varied libvips allocation patterns

---

## 6. Caching Design

### 6.1 Strategy

| Layer | TTL | Mechanism |
|-------|-----|-----------|
| CloudFront | 7 days | `Cache-Control: public, max-age=604800` |
| ETag revalidation | Indefinite | `ETag` over transform params + image content hash |
| Client browser | 7 days | Inherits CloudFront headers |

**ETag construction:** `FNV-1a64(transform_options) + SHA256(output_bytes)` — deterministic per `(image version, transform)` pair. On revalidation, Imago returns `304 Not Modified` with no re-processing if the ETag matches.

### 6.2 Cache Key

CloudFront cache key = full URL path + all query params:

```
/users/123/photo_v2.jpg?w=512&h=512&format=webp&quality=85
```

Each unique `(s3_key, transform combination)` is a separate cache entry. No `Vary` header — format is explicit via `?format=` param, not `Accept` header negotiation.

### 6.3 Cache Invalidation

**Strategy: versioned S3 keys — no explicit CloudFront invalidation needed.**

When a user re-uploads a photo:
1. A new S3 key is generated: `users/123/photo_v3.jpg`
2. The application backend stores and serves the new key
3. Old URLs (`photo_v2.jpg`) expire naturally after 7 days
4. No CloudFront `CreateInvalidation` API call needed — zero cost, zero propagation delay

This avoids CloudFront invalidation costs ($0.005/path) and the ~15s propagation window during which stale content is served.

---

## 7. API Design

### 7.1 Conventions

| Convention | Value |
|------------|-------|
| Version prefix | `/v1/` (upload endpoints only) |
| Serve URL | `/{s3_key}?params` (no version prefix — CDN-cacheable) |
| Param style | Short for dimensions (`w`, `h`), long for everything else |
| Error format | `{ "code": "...", "message": "...", "field": "..." }` |
| Success content-type | `image/webp`, `image/jpeg`, `image/png` |
| Error content-type | `application/json` |

### 7.2 Upload Endpoints

#### `POST /v1/upload/presign`
Request a presigned S3 URL for direct client-to-S3 upload (Path A).

**Request:**
```json
{ "filename": "photo.jpg", "content_type": "image/jpeg", "size_bytes": 4200000 }
```

**Response:**
```json
{
  "presigned_url": "https://s3.amazonaws.com/bucket/users/123/photo_v2.jpg?X-Amz-...",
  "s3_key": "users/123/photo_v2.jpg",
  "expires_in": 300
}
```

#### `POST /v1/upload/confirm`
Confirm that a direct S3 upload completed. Server performs `HeadObject` to verify.

**Request:**
```json
{ "s3_key": "users/123/photo_v2.jpg" }
```

**Response:**
```json
{ "s3_key": "users/123/photo_v2.jpg", "size_bytes": 4200000, "ready": true }
```

#### `POST /v1/upload/binary`
Server-side upload with EXIF stripping and pre-resize (Path B).

**Request:** `Content-Type: image/jpeg` — raw binary body (or `multipart/form-data` with `file` field)

**Response:**
```json
{ "s3_key": "users/123/photo_v2.jpg", "size_bytes": 4200000 }
```

Processing applied:
- Magic byte validation (reject forged content types)
- EXIF / IPTC / XMP strip
- HEIC → JPEG transcode if needed

### 7.3 Serve Endpoints

#### `GET /{s3_key}`
Return image with optional transforms. All params are optional — omitting them returns the original stored image.

**URL examples:**
```
/users/123/photo_v2.jpg
/users/123/photo_v2.jpg?w=512
/users/123/photo_v2.jpg?w=512&h=512&fit=cover
/users/123/photo_v2.jpg?w=160&h=160&fit=cover&format=webp&quality=85
```

**Query parameters:**

| Param | Type | Description |
|-------|------|-------------|
| `w` | int | Target width in pixels |
| `h` | int | Target height in pixels |
| `fit` | string | `contain` (default), `cover`, `fill` |
| `format` | string | `jpeg`, `webp`, `png` |
| `quality` | int | Encode quality 1–100 (default 85) |
| `crop` | string | `center`, `top`, `bottom`, `left`, `right`, `smart` |

**Response headers:**
```
Content-Type: image/webp
Cache-Control: public, max-age=604800
ETag: "a3f8c2d1..."
```

#### `GET /v1/info/{s3_key}`
Return image metadata as JSON without any transform.

**Response:**
```json
{ "width": 2048, "height": 1365, "format": "jpeg", "size_bytes": 812000 }
```

### 7.4 Error Response Format

All errors return `application/json`:

```json
{
  "code": "INVALID_PARAM",
  "message": "width must be between 1 and 5000",
  "field": "w"
}
```

| HTTP Code | Code | Meaning |
|-----------|------|---------|
| 400 | `INVALID_PARAM` | Bad query parameter |
| 400 | `INVALID_IMAGE` | Corrupt or unsupported image format |
| 413 | `IMAGE_TOO_LARGE` | Exceeds size or pixel limit |
| 404 | `NOT_FOUND` | S3 key does not exist |
| 503 | `OVERLOADED` | Worker queue full, retry after backoff |

---

## 8. Scaling

### 8.1 ECS Service Configuration

| | imago-uploader | imago-processor |
|-|---------------|-----------------|
| ALB rule | `/v1/upload/*` | `/*` |
| Instance type | `c7g` (Graviton3) | `c7g` (Graviton3) |
| Scale metric | CPU + RAM | CPU + RAM |
| Min tasks | Configurable per env | Configurable per env |
| Max tasks | Configurable per env | Configurable per env |

### 8.2 Scaling Rationale

**Why `c7g` Graviton3:**
- libvips has excellent ARM NEON SIMD support for JPEG DCT, WebP transforms, and convolution
- ~20–30% better throughput per dollar than equivalent x86 (`c6i`) for image workloads
- Lower per-vCPU cost than x86 compute-optimized instances

**Why CPU + RAM as dual scaling metric:**
- CPU covers compute-intensive transforms (WebP encode, smart crop)
- RAM covers concurrent large-image loads (multiple 4K images in-flight simultaneously)
- Either metric breaching threshold triggers scale-out

### 8.3 Backpressure

When the `VipsWorkerPool` queue is full, imago returns `HTTP 503` with `Retry-After: 1`. This gives the ALB and upstream callers a clear signal to back off rather than timing out silently, preventing cascading failures under burst load.

---

## 9. Deployment

### 9.1 Docker Image

```dockerfile
FROM ubuntu:24.04
RUN apt-get install -y libvips libvips-dev libjemalloc2
COPY imago /usr/local/bin/imago
ENV LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libjemalloc.so.2
ENTRYPOINT ["/usr/local/bin/imago"]
```

- Base: Ubuntu 24.04 (LTS, `arm64` for `c7g`)
- libvips installed from system packages
- jemalloc loaded via `LD_PRELOAD` — reduces C-heap fragmentation under libvips workloads
- Image stored in Amazon ECR

### 9.2 Configuration (Environment Variables)

| Variable | Default | Meaning |
|----------|---------|---------|
| `S3_BUCKET` | — | S3 bucket name |
| `S3_REGION` | — | AWS region |
| `CLOUDFRONT_ORIGIN_TOKEN` | — | Secret header value validated on every request |
| `VIPS_WORKER_COUNT` | `0` (= CPU count) | VipsWorkerPool thread count |
| `MAX_INPUT_BYTES` | `20971520` | 20 MB upload limit |
| `MAX_INPUT_PIXELS` | `50000000` | 50 MP pixel limit |
| `CACHE_TTL` | `604800` | Cache-Control max-age in seconds |
| `ALLOWED_URL_DOMAINS` | — | Comma-separated domain whitelist for `?url=` source |

### 9.3 CI/CD Pipeline

```
git push → GitHub Actions
    │
    ├── cmake build (arm64 cross-compile or native runner)
    ├── ctest (unit + integration tests)
    ├── docker build → ECR push
    └── ECS rolling deploy (imago-uploader + imago-processor)
         · Max 100% healthy, min 50% — zero downtime
         · Health check: GET /health → 200 before traffic shifted
```

---

## 10. Monitoring

### 10.1 Observability Stack

| Signal | Tool |
|--------|------|
| Metrics | Prometheus (Imago exposes `/metrics`) + Amazon Managed Grafana |
| Logs | CloudWatch Logs (ECS ships logs automatically) |
| Alarms | CloudWatch Alarms on key thresholds |

### 10.2 Key Metrics

| Metric | Alert threshold |
|--------|----------------|
| `imago_request_duration_p99` | > 300ms |
| `imago_error_rate` | > 1% of requests |
| `imago_worker_queue_depth` | > 80% of max |
| `imago_upload_size_bytes_p99` | Monitor for abuse |
| Container CPU% | > 70% (triggers scale-out) |
| Container RAM% | > 75% (triggers scale-out) |
| CloudFront cache hit rate | < 85% (investigate cache key issues) |

### 10.3 Structured Logs

Each request logs a single JSON line:

```json
{
  "ts": "2026-04-15T14:32:01Z",
  "endpoint": "processor",
  "s3_key": "users/123/photo_v2.jpg",
  "transform": { "w": 512, "h": 512, "format": "webp", "fit": "cover" },
  "input_px": "2048x1365",
  "output_bytes": 18420,
  "duration_ms": 87,
  "cache": "miss",
  "status": 200
}
```

### 10.4 Health Check

```
GET /health

{
  "status": "ok",
  "vips_workers": { "total": 8, "busy": 3, "queue": 0 },
  "uptime_s": 86400
}
```

ECS health checks call `/health` every 30s. Container is replaced if 3 consecutive checks fail.

---

## 11. Benchmarking

### 11.1 Competitive Benchmark (Build vs. Adopt Justification)

Run once before finalizing the build decision. Results from the 2026-04-14 run comparing imago vs imgproxy vs weserv/images under identical load (300 requests, 20 concurrent, sequential containers):

| Scenario | imago | imgproxy | weserv |
|----------|-------|----------|--------|
| JPEG resize w=512 (RPS) | 229 | 266 | 28 |
| JPEG resize w=512 (CPU) | **0.12%** | 666% | 73.5% |
| JPEG resize w=512 (RAM) | 357 MB | 333 MB | 1368 MB |
| WebP resize w=512 (RPS) | **267** | 233 | 127 |
| No-enlarge passthrough (RPS) | **792** | 507 | 53 |
| Convert → WebP (RPS) | **144** | 53 | 12 |
| Worst p99 latency | 0.66s | ~5s | ~5.4s |
| Info endpoint small (RPS) | **7031** | — | — |

**Key finding:** imago achieves comparable or superior throughput to imgproxy while using 5500× less CPU per request and 4–8× less memory than weserv. No open-source solution handles the upload path (EXIF strip, pre-resize) — that gap requires building regardless.

### 11.2 Internal Benchmark (Pre-Deploy Checklist)

Run before each major release and before any production instance type change.

**Tool:** `hey` (same as competitive benchmark)  
**Load:** 300 requests, 20 concurrent per scenario  
**Scenarios:**

| Group | Scenarios |
|-------|-----------|
| Upload | Binary upload 1MB / 4MB / 10MB JPEG |
| Resize | JPEG w=256, w=512, w=1024; WebP w=512; PNG w=512 |
| Crop | 512×512 centre, smart crop, 800×600 |
| Convert | → WebP, → PNG, → JPEG q=50 |
| Cache | ETag 304 round-trip |
| Passthrough | No-enlarge, info endpoint |

**Pass criteria:**

| Metric | Threshold |
|--------|-----------|
| p99 latency (resize) | < 300ms |
| p99 latency (upload binary 4MB) | < 1000ms |
| Error rate | 0% |
| RSS under sustained load | < 512 MB |
| CPU per request (resize) | < 10% |

Results written to `scripts/bench/results/` as CSV and HTML report. Regression vs. previous run is flagged automatically.

---

*[End of Part 2 — System Architecture, Caching, API, Scaling, Deployment, Monitoring, Benchmarking]*
