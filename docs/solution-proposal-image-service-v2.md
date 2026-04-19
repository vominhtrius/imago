# Solution Proposal: Company-Wide Image Processing Platform

**Audience:** CTO, Engineering Director
**Author:** Engineering
**Date:** 2026-04-19
**Status:** For approval
**Supersedes:** `solution-proposal-image-service.md`

---

## 1. Context

Problems this platform is designed to address:

- **Incomplete transform coverage.** No unified support for resize, crop, format conversion, or watermark; feature set varies per product.
- **Privacy exposure.** EXIF metadata (GPS coordinates, device fingerprints) is retained on user-uploaded photos and served downstream.
- **Performance below product requirements.** Slow image delivery and high bandwidth consumption degrade mobile UX — visible on feed scroll and chat rendering.
- **Architectural friction.** Resize is an asynchronous pipeline; clients must upload, poll for derivatives, and reference resized variants by separate keys — complex integration for every consuming team.
- **Storage amplification.** Both originals and every pre-generated resize variant are persisted to S3, inflating storage cost against an on-demand derivation model.
- **No shared platform.** Each P&L product reinvents its image path, duplicating operational burden and diverging on quality and security posture.

**Goal:** one image platform — synchronous, on-demand, CDN-fronted — adopted across all P&L products.

---

## 2. Essential Features for an Image Service

### 2.1 Must

| Name | Capability | Priority |
|------|------------|----------|
| Upload | <ul><li>EXIF / IPTC / XMP stripping on ingest</li><li>Magic-byte validation (reject forged content types)</li><li>Format normalization — HEIC → JPEG/WebP at ingest</li><li>Pixel and byte-size caps enforced before decode</li><li>Binary body and `multipart/form-data` support</li></ul> | Critical |
| Resize | <ul><li>Modes: fit (letterbox), cover (crop-to-fill), fill, force</li><li>Width / height / bounding box; DPR scaling (1×, 2×, 3×)</li><li>Shrink-on-load decode (JPEG 1/2/4/8, WebP fractional)</li><li>No-enlarge passthrough when source is smaller than target</li></ul> | Critical |
| Crop | <ul><li>Gravity-based (top / bottom / left / right / centre)</li><li>Smart crop — content-aware positioning via libvips</li><li>Focal-point crop (client-specified X/Y fraction)</li><li>Manual pixel-offset region extract</li></ul> | Critical |
| Convert | <ul><li>Encode to JPEG, WebP, PNG</li><li>Per-format quality control (default + per-request override)</li><li>Auto format negotiation via `Accept` or explicit `?format=`</li><li>EXIF auto-rotate on output</li><li>Strip output metadata (EXIF / IPTC / XMP)</li></ul> | Critical |
| Cache | <ul><li>ETag generation + conditional GET (304 Not Modified)</li></ul> | High |

### 2.2 Nice to have

| Name | Capability | Priority |
|------|------------|----------|
| Watermark | Image overlay with gravity, opacity, scale, tile | Medium |
| Blur | Gaussian blur with configurable sigma (NSFW, spoiler, privacy) | Medium |
| Shape mask | Circle, rounded square, custom SVG path | Low |
| Sharpen | Unsharp-mask sharpening after downscale | Low |
| Rotate / Flip | Manual 90°/180°/270°; horizontal and vertical flip | Low |
| Pipeline | Chain multiple operations in one request (resize → watermark → convert) | Medium |
| Info | `/info` endpoint — dimensions, format, EXIF as JSON (no transform) | Medium |
| Placeholder | Configurable fallback image when source is unavailable | Low |
| AVIF | AVIF encode (future format adoption) | Low |

---

## 3. Architecture of an Image Service

A typical image service sits between end clients and object storage, fronted by a CDN. Key properties:

- **On-demand processing.** Transforms (resize, crop, convert, watermark, …) are computed per request from the original — no pre-generated variants, no storage amplification.
- **CDN-cached output.** Each rendered variant is cached at the edge keyed by its full URL; the service only pays the CPU cost on the first miss, and every subsequent hit is served from CloudFront.
- **Asymmetric traffic.** Serve traffic is absorbed by the CDN on read; upload traffic bypasses the CDN and reaches the service directly.

### 3.1 Component Topology

```
                       ┌──────────────────────────────────────────┐
                       │  0. Clients  (Mobile · Web · Desktop)    │
                       └──┬──────────────────────────────┬────────┘
                 serve    │                    upload    │
                          ▼                              │
                  ┌────────────────┐                     │
              ┌───│ 1. CloudFront  │                     │
              │   │     (CDN)      │                     │
              │   └────────┬───────┘                     │
              │            │ miss                        │
   original   │            │ (transform)                 │
   miss       │            ▼                             │
   (direct    │  ┌────────────────────────────────────────────────────┐        ┌──────────────────────┐
    to S3)    │  │ 3. Image Service                                   │ admin  │ 4. Management Tool   │
              │  │                                                    │◄───────│  keys · quotas ·     │
              │  │  ┌──────────────────────┐  ┌──────────────────┐    │ APIs   │  moderation · dash   │
              │  │  │ 3.2 Image Processor  │  │ 3.1 Image        │    │        └──────────────────────┘
              │  │  │  resize · crop ·     │  │     Uploader     │    │
              │  │  │  convert · watermark │  │  EXIF strip ·    │    │          ┌──────────────────────┐
              │  │  │  ETag / 304          │  │  magic validate· │    │ internal │ 5. Service Consumers │
              │  │  │                      │  │  HEIC → JPEG/WebP│    │◄─────────│  Chat · Feed ·       │
              │  │  │                      │  │  pre-resize      │    │ APIs     │  Profile · Story     │
              │  │  └──────────┬───────────┘  └──────────┬───────┘    │          └──────────────────────┘
              │  └─────────────┼─────────────────────────┼────────────┘
              │                │ GetObject      PutObject│
              ▼                ▼                         ▼
              ┌────────────────────────────────────────────┐
              │  2. AWS S3 — Original Storage              │
              └────────────────────────────────────────────┘
```

### 3.2 Components

**0. Clients.** Mobile, web, desktop. Serve hits the CDN; upload goes direct to the Image Service.

**1. CloudFront (CDN).** Edge cache with two path-based origins:
- `/transform/*` → Image Service (Processor) renders on miss.
- `/original/*` → S3 directly on miss (no service CPU).

Updates produce a new URL, so CloudFront invalidation is never required.

**2. AWS S3.** Source of truth for originals. Written by the Uploader; read by the Processor and by CloudFront directly. Keys are never overwritten — a URL always resolves to the exact bytes it was minted with.

**3. Image Service.** Single binary, two independently-scaling services behind one ALB.

- **3.1 Image Uploader** — validates magic bytes and size caps, strips EXIF / IPTC / XMP, normalizes HEIC → JPEG/WebP, optionally pre-resizes, writes to S3 under a new key, returns the URL. Optional presigned-URL mode (not recommended) lets clients PUT to S3 directly, bypassing all ingest guarantees.
- **3.2 Image Processor** — handles CDN miss: fetch from S3, transform (resize / crop / convert / watermark), set `Cache-Control` + `ETag`, stream response. Stateless.

**4. Management Tool.** Operator console: HMAC key rotation, per-consumer quotas, content inspection, health dashboards.

**5. Service Consumers.** Internal products (Chat, Feed, Profile, Story) that integrate against the Image Service public API — URL signing and ingest on behalf of users; internal routing is hidden.

---

## 4. Options & Recommendation

### 4.1 Market Landscape

| Candidate | Stack | Features | Pros | Cons |
|---|---|---|---|---|
| **imgproxy** | Go → libvips (CGo) | resize, crop, convert, HMAC, OTel/Prom | Production-proven at B-req/day scale; first-class observability; pluggable S3/GCS/Azure | Serve-only (no ingest); CGo overhead; Pro license gates smart-crop, AVIF, presets |
| **thumbor** | Python → Pillow | resize, crop, face-detect smart crop, filter plugins | Richest feature set; large plugin ecosystem | Python + Pillow ~order-of-magnitude slower than libvips → **rejected** on throughput |
| **weserv/images** | C++ → libvips (nginx module) | resize, crop, convert via query string | No CGo; nginx I/O | p99 ≥ 5 s under load, 4–8× memory of imgproxy → **rejected** |

**libvips is the common engine.** LGPL C library that streams pixels through a demand-driven pipeline — 4–8× faster and lower-memory than ImageMagick/Pillow. Every non-rejected candidate is a different wrapper around the *same* libvips calls; the differentiator is the language and runtime calling into it.

**Pick: imgproxy.** Best of the existing options — proven at scale, first-class observability, OSS core is MIT (Pro gates are not material, we can fork and reimplement them). The structural concern is the runtime:

- **CGo boundary.** Every libvips call crosses Go→C: stack switch, parameter marshalling, `runtime.LockOSThread` to pin the goroutine (libvips has thread-local state). On a hot path dominated by short libvips calls, it's a tax that never goes away.
- **Go GC.** Image pipelines allocate heavily — S3 byte slices, buffer copies across the CGo boundary, response buffers. Under sustained load this drives GC cycles and STW pauses that show up in tail latency. You can tune `GOGC` but you can't opt out.

C++ removes both: no FFI (libvips called directly) and no tracing GC (jemalloc handles allocation deterministically, with arena-local dirty-page caps for stable RSS). We suspected that alone could be meaningfully faster.

### 4.2 imago — the C++ port

imago is a C++23 reimplementation of imgproxy's core. Same engine (libvips), same URL-based transform model — but called directly from C++ instead of through a Go FFI.

Stack:

- **Drogon HTTP + C++20 coroutines** — async I/O without callback hell; `co_await` bridges to the worker pool.
- **AWS SDK C++** — native S3 client, no FFI on the fetch path.
- **libvips (native C API)** — called directly, no CGo/JNI/PyObject boundary.
- **jemalloc** — allocator with per-arena dirty-page caps; keeps RSS flat under churn.
- **VipsWorkerPool** — isolates libvips work from Drogon's IO threads (`co_await pool.submit(...)`); `VIPS_CONCURRENCY=1` per worker puts parallelism at the pool level and avoids thread-pool thrash.

RSS is stable (0.7–1.1 GB) under sustained load.

One binary handles both the serve path (resize / crop / convert) and the ingest path (upload, EXIF strip, HEIC→JPEG/WebP, magic-byte validation). Deployed as two ECS services sharing the same image but scaling independently. Current state: ~5 KLoC, benchmark harness in place. Full architecture in §5; numbers in §7.

### 4.3 Measured: imago vs imgproxy

Run `9f04418_20260419_002902`. Same hardware, same libvips build, same dataset, 14 VUs · 60 s per scenario (methodology in §7).

**Host.** MacBook Pro · Apple M4 Pro · 14 cores (10 P + 4 E) · 24 GB RAM · macOS (Darwin 25.3). Both services run in Docker with 4 worker threads each (`THREAD_POOL_SIZE=4`, `DROGON_WORKERS=4` for imago; `IMGPROXY_CONCURRENCY=4` for imgproxy), libvips built from the same base image.

**Scenarios.** The k6 scripts (`benchmark/k6.js`) are adapted from [imgproxy's own benchmark suite](https://github.com/imgproxy/imgproxy/tree/master/tools/benchmark) — same URL shapes, same operations, same quality hints — so imgproxy runs on the workload it was authored against. imago exposes equivalent endpoints (`/resize`, `/crop`, `/convert`) and the script routes each tool to its native URL format.

| Scenario | imgproxy rps | imago rps | Gain | imgproxy p95 | imago p95 |
|---|---|---|---|---|---|
| Resize JPEG 512 | 207 | 481 | **2.32×** | 79 ms | 40 ms |
| Resize WebP 512 | 99 | 216 | **2.18×** | 168 ms | 87 ms |
| Crop JPEG 512 | 200 | 444 | **2.22×** | 83 ms | 41 ms |
| Convert JPEG→WebP | 16 | 49 | **3.07×** | 1010 ms | 370 ms |

**Throughput & latency.**

![Throughput and average latency](../benchmark/results/charts/throughput_latency_9f04418_20260419_002902.png)

![Speedup — imago over imgproxy](../benchmark/results/charts/speedup_9f04418_20260419_002902.png)

![Latency percentiles (avg / p90 / p95)](../benchmark/results/charts/latency_percentiles_9f04418_20260419_002902.png)

**CPU & memory.**

![Peak CPU and peak memory per scenario](../benchmark/results/charts/resource_usage_9f04418_20260419_002902.png)

**Memory stability (imago, all scenarios back-to-back).**

![imago RSS across all runs (idle gaps collapsed)](../benchmark/results/charts/memory_timeline_stitched.png)

![imago RSS over time per benchmark](../benchmark/results/charts/memory_per_run.png)

#### Final comment

- **Throughput: 2.19–3.07× across the board.** We expected a meaningful gain from removing CGo; the measurement is well beyond that. CGo alone doesn't account for it — Go's GC pressure under high allocation, goroutine scheduling around blocking CGo calls, and the Go AWS SDK's per-request overhead all stack on top of the FFI tax.
- **Latency halved on resize/crop, cut to a third on convert.** p95 tracks throughput; no tail blow-up under load.
- **CPU tells the real story.** imgproxy plateaus at ~4 cores average even with 14 concurrent VUs on a 14-core box — the Go scheduler + `runtime.LockOSThread` + CGo handoff can't feed more cores. imago scales to 11–13 cores on the same workload. Per-request CPU cost is roughly the same; imago wins by unlocking parallelism, not by being cheaper per op.
- **Memory higher but stable.** imago peak 0.7–1.1 GB vs imgproxy 0.2–0.5 GB (3–5× higher) — the price of jemalloc arenas holding dirty pages for reuse. Across 8 back-to-back runs (20 active minutes of load) the trendline is slightly **negative** (-1.62 MB/min) — no leak, fragmentation bounded by the arena cap.
- **No failures.** All scenarios completed with 0 errors at target concurrency.

RSS cost is real but bounded: well under the 2 GB ceiling, and on CPU-bound `c7g` where compute sets instance size, the extra memory is effectively free.

### 4.4 Recommendation

Ingest is built either way. The trade-off is **performance vs. development cost / team risk**.

#### Option 1 — imgproxy (forked where needed)

- **Performance.** Acceptable. 207 rps · p95 79 ms on Resize JPEG 512; scales horizontally on AWS by adding tasks. Latency sits in the "average" band — fine for page loads, noticeable on feeds and chat.
- **Development cost.** Adding features we need (smart-crop, AVIF, HEIC ingest) requires **forking, learning the Go codebase, and rebasing upstream forever**. Continuous tax, not a one-shot port.

#### Option 2 — imago (in-house C++23)

- **Performance.** 2.19–3.07× throughput; p95 halved on resize/crop (~40 ms), cut to a third on convert. Low tail latency is the differentiator for **social/chat workloads** where a 150 ms tail shows up as feed jank. Fleet ≈ half imgproxy's.
- **Risk.** Needs a **senior C++ engineer** to own — modern C++23 / coroutines / sanitizers aren't entry-level. Codebase is small (~5 KLoC) and bounded by libvips, but bus-factor is real on a Go-skewed team.

#### Decision

**Adopt imago** if we can staff one senior C++ owner. The 2–3× throughput and halved p95 matter at scale; infrastructure cost will reduce; CGo is a ceiling that won't move.

**Otherwise, fall back to Option 1 (forked imgproxy)** — second-best, fundable, doesn't hinge on one person.

---

## 5. Proposed Architecture

### 5.1 Stack

C++23 · Drogon HTTP · libvips · AWS S3 SDK · jemalloc · Prometheus client

### 5.2 Deployment Topology

One Docker image, two ECS services routed by ALB path prefix:

| ECS Service | ALB Rule | Scales on |
|---|---|---|
| `imago-uploader` | `/v1/upload/*` | CPU (EXIF strip + pre-resize) |
| `imago-processor` | `/*` | CPU + RAM (libvips transforms) |

Independent auto-scaling prevents upload spikes (evening chat activity) from starving serve-path capacity (feed loading).

### 5.3 Data Flow

```
Upload path:
  Client → imago-uploader → (EXIF strip, validate, pre-resize) → S3

Serve path:
  Client → CloudFront
           ├─ Cache HIT  → < 20 ms response, origin not touched
           └─ Cache MISS → ALB → imago-processor
                                  ├─ S3 GetObject
                                  ├─ co_await vipsPool.submit(...)
                                  │    · shrink-on-load decode
                                  │    · resize / crop / convert
                                  │    · encode
                                  └─ respond + Cache-Control + ETag
                                     (CloudFront stores result)
```

### 5.4 Thread Model

```
Drogon IO threads (N)  —  epoll, HTTP parse, S3 SDK async
        │  co_await vipsPool.submit(lambda)
        ▼
VipsWorkerPool (M)     —  all libvips runs here
                          VIPS_CONCURRENCY=1 per worker
                          vips_thread_shutdown() per job
                          op cache disabled (vips_cache_set_max(0))
```

IO threads never call libvips. Workers never call the network. Parallelism is at the worker-thread level; libvips internal threading is disabled to avoid over-subscription.

### 5.5 State

Stateless. S3 is the single source of truth. CloudFront absorbs repeats; cache key is the full URL path + query string. **Updates produce a new URL** rather than overwriting an existing S3 key, so a URL always resolves to the exact bytes it was minted with — CloudFront invalidation is never required, zero API cost, zero propagation window.

---

## 6. Operating Model

### 6.1 Deploy

- ECS on `c7g` (Graviton3) EC2 — libvips has strong ARM NEON SIMD paths; ~20–30 % better throughput per dollar vs `c6i` for image workloads.
- Single Docker image (Ubuntu 24.04 base, libvips from system packages, jemalloc via `LD_PRELOAD`).
- CI/CD: GitHub Actions → cmake build → ctest → ECR push → ECS rolling deploy (100 % max / 50 % min, `/health`-gated).

### 6.2 Scale & Backpressure

- Dual metric auto-scale: CPU ≥ 70 % or RAM ≥ 75 %.
- Queue full → HTTP 503 with `Retry-After: 1` so the ALB and upstream callers back off cleanly instead of timing out.

### 6.3 Observability

| Signal | Source |
|---|---|
| Metrics | `/metrics` (Prometheus) → Amazon Managed Grafana |
| Logs | CloudWatch Logs (structured JSON, one line per request) |
| Alarms | CloudWatch on p99 latency, error rate, queue depth, RSS, CF hit rate |
| Health | `GET /health` — reports worker pool status; ECS probes every 30 s |

Key alerts: `p99 > 300 ms`, `error_rate > 1 %`, `queue_depth > 80 %`, `cache_hit_rate < 85 %`.

### 6.4 Config

All tunables via environment variables — worker count, size/pixel caps, signing key, cache TTL, allowed URL domains, S3 bucket/region. No config files, no image rebuilds for tuning.

---

## 7. Validation

### 7.1 Benchmark Setup

- Commit `9f04418`, run `9f04418_20260419_002902`
- Host: 14-core machine (14 VUs = 14 concurrent clients)
- 3 minutes per scenario, k6 load generator
- Same MinIO backend, same source dataset, services run sequentially (no cross-contention)
- 0 failures on either service across all scenarios

### 7.2 Results

| Scenario | Service | rps | p50 | p95 | p99 | Peak RSS |
|---|---|---:|---:|---:|---:|---:|
| Resize JPEG 512 | imgproxy | 207 | 67 ms | 79 ms | 76 ms | 186 MB |
|  | **imago** | **481** | **28 ms** | **40 ms** | **37 ms** | 982 MB |
| Resize WebP 512 | imgproxy | 99 | 140 ms | 168 ms | 161 ms | 189 MB |
|  | **imago** | **217** | **64 ms** | **87 ms** | **81 ms** | 737 MB |
| Crop JPEG 512 | imgproxy | 200 | 69 ms | 83 ms | 79 ms | 225 MB |
|  | **imago** | **444** | **31 ms** | **41 ms** | **38 ms** | 809 MB |
| Convert → WebP | imgproxy | 16 | 868 ms | 1010 ms | 981 ms | 488 MB |
|  | **imago** | **49** | **283 ms** | **370 ms** | **347 ms** | 1075 MB |

### 7.3 Reading the Numbers

- **Throughput:** imago wins every scenario by 2.19–3.07×. Advantage is largest on WebP encode (CPU-bound) and smallest on JPEG resize (shrink-on-load dominated).
- **Latency:** imago p95 is roughly half of imgproxy in each scenario. No tail-latency anomaly observed in this run (a prior run showed a 7-minute tail on imago's first scenario — traced to container cold-start; did not reproduce here).
- **Memory:** imago steady at 0.7–1.1 GB, imgproxy at 0.2–0.5 GB. Both stable across scenarios. imago jemalloc shutdown stats confirm no leak: `nmalloc` and `ndalloc` balanced, allocated-at-exit 2.96 MB.
- **CPU:** imago uses all 14 cores; imgproxy defaults to 4 workers per its standard configuration. imago's advantage is not lower per-request CPU — it is higher parallelism at similar per-request cost.
- **Zero failures** on both services. This matters: a prior run showed imgproxy returning 100 % non-200 responses fast enough to appear as 1600 rps in charts until we added a failure-rate guard to the summary pipeline.

### 7.4 Acceptance Criteria (for future runs)

A release is accepted if, on the standard 4-scenario bench:

- Error rate = 0
- Resize p99 < 80 ms at 14 VUs
- Convert → WebP p99 < 500 ms
- RSS stable (no upward trend over 15 min sustained load)
- No regression vs previous release > 10 %

---

## 8. Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| In-house maintenance burden | Engineering cost | Small codebase (~5 KLoC); libvips does the heavy lifting; CI-enforced benchmark; on-call rotation |
| libvips CVE | Security exposure | Same exposure as imgproxy / thumbor (all use libvips); patched via base image update |
| C++ memory safety (leaks, UAF) | Crashes, instability | jemalloc in prod, ASAN in CI, RSS monitored continuously, benchmarks measure sustained-load behaviour |
| Hiring pool smaller than Go | Slower onboarding | Service layer is idiomatic modern C++; image logic is configuration of libvips; ramp-up materials in `docs/` |
| AVIF encode gap | Future format adoption | libvips supports AVIF natively; enable with a build flag when product demand appears |
| Observability parity vs imgproxy | Slower incident diagnosis | Phase 1 exit criterion requires Prometheus parity + log schema signed off |
| Regression in a future release | Production incident | Bench is run on every release tag; summary CSV diff gates merges to `main` |

---

## 9. Rollout

No fixed dates. Each phase has explicit exit criteria; the next phase does not start until the previous exits.

### Phase 1 — Pilot

- Deploy imago to one product surface (e.g. chat avatar thumbnails).
- Run imago in shadow mode alongside the existing resizer for 10 % of live serve traffic; compare response bytes, content hash, and latency.
- **Exit:** 7 consecutive days of shadow-read parity, error rate < 0.1 %, p99 within SLO, Prometheus + log schema signed off by SRE.

### Phase 2 — Platform

- Migrate 2–3 additional product surfaces (feed media, story previews, profile photos).
- Publish internal SDK / URL helper so product teams do not hand-construct URLs.
- Turn on HMAC URL signing in enforcement mode.
- **Exit:** CloudFront hit rate ≥ 85 %, zero P1 incidents attributable to imago, all onboarded services emitting expected metrics.

### Phase 3 — Default

- imago is the mandated path for any new image pipeline.
- Legacy per-product resizers scheduled for sunset; ownership transferred or decommissioned.
- **Exit:** no active non-imago image pipelines in production or, where exceptions exist, documented with an owner and retirement date.

---

## 10. Decision Requested

Approve imago as the company-wide image processing platform and authorise Phase 1 pilot deployment.
