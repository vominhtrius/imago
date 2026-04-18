# Image Service — Solution Proposal

**For:** CTO Review  
**Date:** 2026-04-15

---

## The Problem

Every image sent in our app today arrives at the client in its original form — full resolution, full size, with metadata intact.

- A photo from an iPhone is **8–12 MB at 4000×3000 px**. A chat thumbnail needs 160×160.
- Serving the original wastes **99% of the bandwidth** on every message.
- GPS and device metadata embedded in EXIF is **sent to every recipient**.
- On a slow 4G connection, images in feed take **3–5 seconds** to appear.

We need a dedicated image service sitting between storage and clients.

---

## What It Does

```
User uploads photo
       ↓
  [ Image Service ]
  · Strip EXIF / metadata
  · Validate & sanitize
  · Store clean original
       ↓
Client requests image
       ↓
  [ Image Service ]
  · Resize to requested dimensions
  · Crop to fit UI slot
  · Convert to WebP / JPEG
       ↓
  Client receives optimized image
```

---

## Core Features (MVP)

### 1. Upload
The entry point. Every image is sanitized before storage.

- Strip all EXIF metadata (GPS, device info, timestamps)
- Validate file format via magic bytes — reject forged content types
- Cap input size (e.g., 20 MB max) and pixel count (e.g., 50 MP max) before decode
- Pre-resize originals to a max dimension (e.g., 2048 px) at ingest — no need to store 4000×3000 forever
- Support both multipart form upload (web) and raw binary POST (mobile SDK)

**Why at upload, not at serve:** Stripping EXIF and resizing once at write time means stored files are always clean. Doing it at serve time means the raw sensitive file exists on disk indefinitely.

---

### 2. Resize
The most frequent operation. Every feed thumbnail, avatar, and preview is a resize.

- Scale to target width, height, or bounding box
- Preserve aspect ratio by default
- Three modes: `fit` (letterbox), `fill` (crop to fill), `force` (stretch)
- Reject upscaling when source is smaller than target (passthrough)

**Impact:** Dropping from a 4 MB JPEG to a 160×160 WebP thumbnail reduces payload by **~98%**.

---

### 3. Crop
Extract the right region for fixed-size UI slots.

- Gravity-based anchor: centre, top, bottom, left, right
- Smart crop (content-aware): automatically selects the most important region — faces stay in frame
- Manual pixel offset for precise extraction

**Use cases:** Avatar circles, story previews, feed grid tiles — all require exact dimensions.

---

### 4. Convert
Serve the right format for each client.

- Transcode between JPEG, WebP, PNG
- Auto-negotiate from `Accept` header — serve WebP to clients that support it, JPEG as fallback
- Configurable quality (JPEG 1–100, WebP quality)
- Strip metadata on output

**Impact:** WebP delivers **25–35% smaller files** than JPEG at equivalent visual quality. For a feed loading 30 images, that is one fewer image worth of data.

---

## Optional Features

These are not required for launch but can be added incrementally:

| Feature | When to add |
|---------|-------------|
| Watermark | Brand overlays, copyright protection |
| Blur | NSFW/spoiler flagging, background defocus |
| Pipeline (multi-op per request) | Complex transforms without round trips |
| AVIF / JXL format support | Next-gen compression for supporting clients |
| Rotate / Flip | User-facing editing tools |
| Sharpen | Recover detail after aggressive downscale |
| ETag / 304 Conditional GET | Reduce redundant transfers for stable images |
| Color adjustments | Story filters, brightness/contrast |
| Shape mask | Circle avatar output without client-side clipping |
| Placeholder on error | Prevent broken image icons in feeds |

---

## Market Options vs. Build

Three established open-source solutions exist:

| | imgproxy | imaginary | weserv/images |
|-|----------|-----------|---------------|
| Upload endpoint | No | Yes (POST body) | No |
| Resize | Yes | Yes | Yes |
| Crop + Smart crop | Pro only | Yes | Yes |
| Convert / Auto-format | Yes | Yes | Yes |
| Memory (RSS) | ~350 MB | ~400 MB | **1400–2800 MB** |
| Tail latency (p99) | Good | Moderate | Poor (≥ 5s on many ops) |
| EXIF strip at upload | No | No | No |
| Pre-resize at upload | No | No | No |
| Active maintenance | Yes | Minimal | Yes |

**The gap:** None of the three handle upload with EXIF stripping and pre-resize. They are transform-only — the upload problem is always left to the application layer.

---

## Proposed Approach

Build a lightweight service with the four MVP features above, designed around two principles:

**1. Upload is a first-class operation, not an afterthought.**  
Clean, resize, and validate at write time. Everything downstream benefits automatically.

**2. Serve path is stateless and cacheable.**  
Every transform response carries correct `Cache-Control` headers. A CDN or nginx cache in front absorbs repeat requests — the service only processes each unique (image, transform) pair once.

The MVP is achievable as a single deployable binary with no external dependencies beyond the image processing library.

---

## Design Principle: On-Demand Synchronous Processing

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

**Why this matters:**

- An `<img src="...">` tag or a mobile `ImageView` cannot poll. The OS expects a single HTTP request that returns image bytes. An async pipeline requires a separate notify-then-reload flow in every client on every platform.
- A user scrolling a feed has ~300ms before a blank tile is noticeable. Async jobs add queue wait time, scheduling, and notification round trips on top of processing time — impossible to fit in that window.
- On-demand fits CDN caching perfectly. The first request computes the result; every subsequent request is served from cache in < 20ms. The service only ever sees cache misses.
- No persistent state. No job records to store, no status to poll, no completed jobs to clean up.

**What makes it viable:**

On-demand only works if two conditions are met:

1. **Processing is fast** — resize must complete in under 300ms. This drives the choice of image library, shrink-on-load optimization, and worker pool sizing.
2. **Cache absorbs repeat requests** — each unique `(image + transform)` pair is computed once, then cached at CDN or nginx. At 90% cache hit rate, the service handles only 10% of actual traffic volume.

**The one exception — batch backfill:**

Generating a new thumbnail size for millions of existing images is a background job, not a user request. That is the only case for an async queue. All user-facing operations — upload and serve — are synchronous.

---

## Expected Impact

| Metric | Before | After |
|--------|--------|-------|
| Average image payload (chat thumb) | ~4 MB | ~40 KB |
| Bandwidth per 1M image requests | ~4 TB | ~40 GB |
| EXIF / GPS exposure | On every image | Stripped at upload |
| Feed image load time (4G) | 3–5 s | < 300 ms |
| Server instances needed for image processing | Embedded in app server | Dedicated, horizontally scalable |
