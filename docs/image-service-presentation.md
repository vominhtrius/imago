# Image Processing Service
### Solution Proposal — CTO Review

---

## Slide 1 — The Problem

Images are the primary medium in chat and social apps — and they are served inefficiently today.

| Reality | Impact |
|---------|--------|
| iPhone photo = 8–12 MB, 4000×3000 px | Chat thumbnail needs 160×160 |
| Original served to every client | 99% of bandwidth wasted per image |
| GPS + device info in EXIF | Leaked to every recipient |
| No format negotiation | iOS sends HEIC, Android receives nothing |
| Feed loads 20–30 images | 3–5s on 4G = users leave |

**Without a dedicated image service, all of this complexity falls onto the client — the worst possible place for it.**

---

## Slide 2 — What Good Looks Like

```
User uploads photo                    Client requests image
        ↓                                     ↓
  Image Service                        CloudFront CDN
  · Validate & strip EXIF              · Cache HIT → < 20ms ✓
  · Store clean original               · Cache MISS ↓
        ↓                              Image Service
      S3                               · Resize to 512×512
                                       · Convert to WebP
                                       · Return + cache
                                             ↓
                                       Client receives 40 KB
                                       instead of 8 MB
```

**Result: 98% bandwidth reduction per image. EXIF never stored. Feed loads in < 300ms.**

---

## Slide 3 — Core Features (MVP)

| Feature | Description |
|---------|-------------|
| **Upload — EXIF strip** | Strip GPS, device info, timestamps on ingest. Stored files are always clean. |
| **Upload — Direct to S3** | Client gets a presigned URL, uploads directly. Fast, no server bottleneck. |
| **Upload — Server-side** | POST binary through our service for EXIF stripping before storage. |
| **Resize** | Scale to any dimension. Fit, cover, or force. Passthrough if already small. |
| **Crop** | Gravity-based (centre, top, smart). Smart crop keeps faces in frame. |
| **Convert** | JPEG → WebP on request. Explicit `?format=webp` — simple, CDN-friendly. |

**Everything else (watermark, blur, pipeline, AVIF, filters) is optional — add incrementally.**

---

## Slide 4 — Design Principle: Synchronous On-Demand

**Every image request returns bytes in the same HTTP response. No jobs. No polling.**

```
✓  GET /users/123/photo.jpg?w=512&format=webp
   → image bytes in < 300ms

✗  POST /jobs/resize  →  { job_id: "abc" }
   → GET /jobs/abc    →  { status: "pending" }
   → GET /jobs/abc    →  { status: "done", url: "..." }
   → GET that URL     →  image bytes
```

Why synchronous is the only viable model:
- `<img src="...">` tags cannot poll — the browser expects bytes, not a job ID
- A user scrolling a feed has ~300ms before blank tiles are noticeable
- On-demand + CDN cache = each unique transform computed **once**, then free forever
- Zero persistent state — no job records, no cleanup, no failure recovery complexity

**The one exception:** batch backfill of a new thumbnail size for millions of existing images → background job. All user-facing operations are synchronous.

---

## Slide 5 — Why Not an Existing Solution?

Three established open-source options evaluated:

| | imgproxy | imaginary | weserv/images |
|-|----------|-----------|---------------|
| Language | Go | Go | C++17 (nginx module) |
| Throughput JPEG resize | ~266 RPS | moderate | ~28–265 RPS (unstable) |
| CPU per request | **666%** | high | blocking (event loop) |
| Memory (RSS) | ~350 MB | ~400 MB | **1400–2800 MB** |
| p99 latency stability | good | moderate | **≥ 5s on many ops** |
| Upload + EXIF strip | **No** | **No** | **No** |
| Smart crop (free) | Pro only | Yes | Yes |
| Active maintenance | Yes | Minimal | Yes |

**The gap no solution fills: upload with EXIF stripping.**
That requires building regardless — so we build the full service.

---

## Slide 6 — Our Solution: imago

**A single C++23 binary. Two responsibilities. One deployment.**

| Layer | Technology | Why |
|-------|-----------|-----|
| HTTP server | Drogon (C++23, coroutines) | Non-blocking IO, event-loop never touches libvips |
| Image processing | libvips | Fastest open-source image library. SIMD-optimized. |
| Object storage | AWS S3 SDK | Native async integration |
| Memory allocator | jemalloc | Reduces C-heap fragmentation under varied libvips allocations |

**Key architectural advantage over all competitors:**
Drogon IO threads never call libvips. All image processing runs on a dedicated `VipsWorkerPool`. C++20 coroutines connect them — `co_await pool.submit(...)` suspends the IO thread without blocking it.

> imgproxy blocks an OS thread per request. weserv blocks the entire nginx event loop.  
> imago never blocks — IO and CPU are fully decoupled.

---

## Slide 7 — System Architecture

> Full diagram: `docs/architecture-diagram.drawio`

**Upload paths:**

```
Path A — Direct to S3 (fast, no EXIF strip):
  Client → POST /v1/upload/presign → { presigned_url, s3_key }
  Client → PUT image → S3 directly
  Client → POST /v1/upload/confirm → imago HEAD checks S3

Path B — Server-side (EXIF stripped):
  Client → POST /v1/upload/binary → imago strips EXIF → PUT to S3
```

**Serve path (on-demand + cache):**

```
Client → GET /users/123/photo.jpg?w=512&format=webp
       ↓
  CloudFront
  ├── Cache HIT  → response in < 20ms
  └── Cache MISS → X-Origin-Token → ALB → imago-processor
                                           ↓
                                     GetObject from S3
                                     shrink-on-load decode
                                     resize / crop / convert
                                     return + Cache-Control + ETag
                                           ↓
                                     CloudFront caches result
```

**Two ECS services, one Docker image:**

| ECS Service | Handles | Scales on |
|-------------|---------|-----------|
| `imago-uploader` | `/v1/upload/*` | CPU |
| `imago-processor` | `/*` (serve) | CPU + RAM |

---

## Slide 8 — Caching Design

**Goal: imago only processes each unique `(image + transform)` pair once. Ever.**

| Layer | TTL | Mechanism |
|-------|-----|-----------|
| CloudFront | 7 days | `Cache-Control: public, max-age=604800` |
| ETag revalidation | Indefinite | `304 Not Modified` — no re-processing |
| Client | 7 days | Inherits CloudFront headers |

**Cache key = full URL + all query params**
```
/users/123/photo_v2.jpg?w=512&h=512&format=webp&quality=85
```
Each unique transform is a separate cache entry. No `Vary` header — format is explicit, not negotiated.

**Cache invalidation = versioned S3 keys**
```
User re-uploads → new key: photo_v3.jpg
Old URLs expire naturally after 7 days
No CloudFront invalidation API calls needed
Zero cost. Zero propagation delay.
```

---

## Slide 9 — API Design

**Serve URL:**
```
https://images.yourdomain.com/{s3_key}?w=512&h=512&fit=cover&format=webp&quality=85
```

**Upload endpoints:**
```
POST /v1/upload/presign   → { presigned_url, s3_key, expires_in }
POST /v1/upload/confirm   → { s3_key, ready: true }
POST /v1/upload/binary    → { s3_key, size_bytes }        ← EXIF stripped
```

**Query parameters:**

| Param | Example | Notes |
|-------|---------|-------|
| `w` | `?w=512` | Target width |
| `h` | `?h=512` | Target height |
| `fit` | `?fit=cover` | `contain` / `cover` / `fill` |
| `format` | `?format=webp` | `jpeg` / `webp` / `png` |
| `quality` | `?quality=85` | 1–100 |
| `crop` | `?crop=smart` | `center` / `top` / `smart` |

**Error response:**
```json
{ "code": "INVALID_PARAM", "message": "width must be between 1 and 5000", "field": "w" }
```

---

## Slide 10 — Infrastructure

```
                    ┌─────────────────┐
                    │   CloudFront    │  images.yourdomain.com
                    └────────┬────────┘
                             │ cache miss + X-Origin-Token
                    ┌────────▼────────┐
                    │      ALB        │
                    └────┬───────┬────┘
                         │       │
           /v1/upload/*  │       │  /*
                         │       │
              ┌──────────▼─┐  ┌──▼──────────┐
              │  imago-    │  │   imago-    │
              │  uploader  │  │  processor  │
              │  ECS + EC2 │  │  ECS + EC2  │
              │  c7g ARM   │  │  c7g ARM    │
              └──────┬─────┘  └─────┬───────┘
                     │              │
                     └──────┬───────┘
                            │
                    ┌───────▼───────┐
                    │   S3 Bucket   │
                    └───────────────┘
```

**Instance: `c7g` Graviton3 (ARM)**
libvips has excellent ARM NEON SIMD support — 20–30% better image throughput per dollar vs. x86.

**Scaling:** CPU + RAM metrics. Each service scales independently. Min/max configurable per environment.

---

## Slide 11 — Monitoring

| Signal | Tool | Key metrics |
|--------|------|-------------|
| Metrics | Prometheus + Amazon Managed Grafana | RPS, p99 latency, queue depth, error rate |
| Logs | CloudWatch Logs | Structured JSON per request |
| Alarms | CloudWatch Alarms | p99 > 300ms, error rate > 1%, queue > 80% |

**Health check:** `GET /health` — returns worker pool status, not just TCP availability.

**Structured log per request:**
```json
{
  "endpoint": "processor", "s3_key": "users/123/photo_v2.jpg",
  "transform": { "w": 512, "format": "webp" },
  "duration_ms": 87, "output_bytes": 18420, "cache": "miss", "status": 200
}
```

---

## Slide 12 — Benchmark Results

**3-way comparison: imago vs imgproxy vs weserv/images**
_(300 requests, 20 concurrent, sequential containers — 2026-04-14)_

| Scenario | imago | imgproxy | weserv |
|----------|------:|--------:|-------:|
| JPEG resize RPS | 229 | 266 | 28 |
| JPEG resize CPU | **0.12%** | 666% | 73.5% |
| JPEG resize RAM | 357 MB | 333 MB | 1368 MB |
| WebP resize RPS | **267** | 233 | 127 |
| No-enlarge passthrough RPS | **792** | 507 | 53 |
| Convert → WebP RPS | **144** | 53 | 12 |
| Worst p99 latency | **0.66s** | ~5s | ~5.4s |
| Info endpoint RPS | **7031** | — | — |

**Summary:**
- imago matches imgproxy throughput at **5500× less CPU**
- imago uses **4–8× less memory** than weserv
- weserv has unacceptable tail latency (p99 ≥ 5s) on many operations
- No competitor handles upload + EXIF stripping

---

## Slide 13 — Expected Impact

| Metric | Before | After |
|--------|--------|-------|
| Average image payload (chat thumb) | ~4 MB | ~40 KB |
| Bandwidth per 1M image serves | ~4 TB | ~40 GB |
| EXIF / GPS in stored files | Always present | Stripped at upload |
| Feed p99 load time (4G) | 3–5 s | < 300 ms |
| Server CPU for image processing | Shared with app server | Isolated, auto-scaled |
| Cache hit (CDN) | 0% | ~90%+ at steady state |

**CDN egress savings at 10M requests/day: ~360 TB/month → ~3.6 TB/month**

---

## Slide 14 — Phased Delivery

| Phase | Scope | Outcome |
|-------|-------|---------|
| **1 — MVP** | Upload (EXIF strip + presign), Resize, Crop, Convert | Core pipeline live, CDN caching active |
| **2 — Quality** | Smart crop, WebP auto-select, ETag/304, Monitoring dashboards | Bandwidth savings measurable |
| **3 — Features** | Watermark, Blur, Pipeline endpoint, AVIF support | Full feature parity with competitors |
| **4 — Scale** | Benchmark-driven tuning, auto-scaling policies, multi-region | Production hardening |

---

*Full technical specification: `docs/solution-proposal-image-service.md`*  
*Architecture diagram: `docs/architecture-diagram.drawio`*
