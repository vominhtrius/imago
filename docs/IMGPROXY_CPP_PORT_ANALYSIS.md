# imgproxy Internals & C++ Port Analysis

> Audience: engineer porting imgproxy to **C++ + Drogon + libvips + C++ coroutines**.
> Based on imgproxy source at commit `61c01ab7` (master).

---

## Table of Contents

1. [How imgproxy Uses libvips](#1-how-imgproxy-uses-libvips)
   - 1.1 [CGo Bridge Architecture](#11-cgo-bridge-architecture)
   - 1.2 [Initialization & Tuning Parameters](#12-initialization--tuning-parameters)
   - 1.3 [libvips Internal Thread Pool](#13-libvips-internal-thread-pool)
   - 1.4 [Sequential Streaming Pipeline](#14-sequential-streaming-pipeline)
   - 1.5 [Image Loading (Buffer-Based)](#15-image-loading-buffer-based)
   - 1.6 [Resize](#16-resize)
   - 1.7 [Crop](#17-crop)
   - 1.8 [Format Conversion / Save](#18-format-conversion--save)
   - 1.9 [Filters, Composition, Color](#19-filters-composition-color)
   - 1.10 [VipsImage Lifecycle Management](#110-vipsimage-lifecycle-management)
   - 1.11 [Full 15-Step Processing Pipeline](#111-full-15-step-processing-pipeline)
2. [Concurrency Model](#2-concurrency-model)
   - 2.1 [HTTP Server & Connection Limiting](#21-http-server--connection-limiting)
   - 2.2 [Two-Tier Admission Control](#22-two-tier-admission-control)
   - 2.3 [S3 Download Architecture](#23-s3-download-architecture)
   - 2.4 [Why imgproxy Uses LockOSThread Per Request](#24-why-imgproxy-uses-lockosthread-per-request)
   - 2.5 [Request Pipeline End-to-End](#25-request-pipeline-end-to-end)
   - 2.6 [Buffer Pooling](#26-buffer-pooling)
   - 2.7 [Context Propagation & Cancellation](#27-context-propagation--cancellation)
3. [Memory Allocation: jemalloc & tcmalloc](#3-memory-allocation-jemalloc--tcmalloc)
   - 3.1 [imgproxy's Approach](#31-imgproxys-approach)
   - 3.2 [AWS SDK C++ jemalloc Support](#32-aws-sdk-c-jemalloc-support)
4. [CGo Overhead Analysis](#4-cgo-overhead-analysis)
   - 4.1 [Call Frequency Per Request](#41-call-frequency-per-request)
   - 4.2 [Memory Passing Patterns](#42-memory-passing-patterns)
5. [C++ Port: Performance Analysis & Design Guide](#5-c-port-performance-analysis--design-guide)
   - 5.1 [Where CGo Costs Live (and Don't)](#51-where-cgo-costs-live-and-dont)
   - 5.2 [Real Performance Wins in C++](#52-real-performance-wins-in-c)
   - 5.3 [Go Goroutines vs C++ Coroutines: The TLS Problem](#53-go-goroutines-vs-c-coroutines-the-tls-problem)
   - 5.4 [The Rule: Never co_await Inside a vips Pipeline](#54-the-rule-never-co_await-inside-a-vips-pipeline)
   - 5.5 [Drogon Architecture Mapping](#55-drogon-architecture-mapping)
   - 5.6 [libvips C++ Integration Design](#56-libvips-c-integration-design)
   - 5.7 [S3 Download in C++](#57-s3-download-in-c)
   - 5.8 [C++ Coroutine Concurrency Model](#58-c-coroutine-concurrency-model)
   - 5.9 [Memory Management Strategy](#59-memory-management-strategy)
   - 5.10 [Traps & Risks](#510-traps--risks)
6. [Summary Comparison Table](#6-summary-comparison-table)

---

## 1. How imgproxy Uses libvips

### 1.1 CGo Bridge Architecture

All libvips interaction is concentrated in three files:

| File | Lines | Role |
|------|-------|------|
| `vips/vips.h` | 100 | C header: `vips_*_go()` wrapper signatures |
| `vips/vips.c` | 1,119 | C bridge: wraps libvips calls into single-shot functions |
| `vips/vips.go` | 1,006 | Go CGo binding: `Image{VipsImage *C.VipsImage}` struct + methods |

Build flags at `vips/vips.go:3-8`:
```go
#cgo pkg-config: vips
#cgo CFLAGS: -O3
#cgo LDFLAGS: -lm
#include "vips.h"
```

The C bridge is compiled at `-O3`. The design is deliberate: one Go method = one C function = one logical vips operation. No pipeline expression trees or deferred evaluation.

Secondary CGo files:
- `gliblog/gliblog.c` — redirects GLib log messages to Go's logger
- `memory/free_linux.go` — calls `malloc_trim(0)` to shrink the C heap

### 1.2 Initialization & Tuning Parameters

`vips.Init()` at `vips/vips.go:67-173`:

```go
runtime.LockOSThread()   // vips init is thread-affine
C.vips_initialize()      // → vips_init("imgproxy")

// Disable the vips operation cache entirely
C.vips_cache_set_max_mem(0)
C.vips_cache_set_max(0)
// Reason: pipeline is fine-tuned, cache gives no benefit;
// enabled cache causes SIGSEGV on Musl/Alpine

// Set per-operation thread count
if isLambda {
    C.vips_concurrency_set(C.int(runtime.GOMAXPROCS(0)))
} else {
    C.vips_concurrency_set(1)   // Go provides the outer parallelism
}
```

Full tuning parameter table:

| Parameter | Value | Env override | Source |
|-----------|-------|-------------|--------|
| `vips_cache_set_max_mem` | 0 (off) | — | `vips/vips.go:78` |
| `vips_cache_set_max` | 0 (off) | — | `vips/vips.go:79` |
| `vips_concurrency_set` | 1 (non-Lambda) / GOMAXPROCS (Lambda) | — | `vips/vips.go:85-88` |
| Leak check | off | `IMGPROXY_VIPS_LEAK_CHECK` | `vips/vips.go:90-92` |
| Cache trace | off | `IMGPROXY_VIPS_CACHE_TRACE` | `vips/vips.go:94-96` |
| Access mode | `VIPS_ACCESS_SEQUENTIAL` | — | All loaders in `vips/vips.c` |
| Line cache tile height | configurable | — | `vips/vips.c:895-900` |
| JPEG optimize_coding | `TRUE` | — | `vips/vips.c:1005` |
| AVIF effort | `9 - speed` | — | `vips/vips.c:1107` |

A periodic goroutine calls `debug.FreeOSMemory() + C.malloc_trim(0)` every `config.FreeMemoryInterval` seconds to reclaim glibc heap fragmentation (`main.go:94-104`, `memory/free_linux.go:16-20`). See [section 3](#3-memory-allocation-jemalloc--tcmalloc) for why this exists and how to eliminate it.

### 1.3 libvips Internal Thread Pool

#### How it works

libvips uses **GLib's `GThreadPool`** internally. The pool is **per-operation, not global**:

```
vips_concurrency_set(N)
  └─ each vips operation (resize, blur, etc.) gets up to N worker threads
     borrowed from GLib's shared thread pool
```

When an operation like `vips_resize` runs, libvips tiles the image and distributes tiles to worker threads:

```
vips_resize(concurrency=4, 1000px wide image)
  ├─ tile rows 0–249   → worker thread 1  (borrowed from GLib pool)
  ├─ tile rows 250–499 → worker thread 2
  ├─ tile rows 500–749 → worker thread 3
  └─ tile rows 750–999 → calling thread (main, not borrowed)
```

GLib's thread pool is **shared and reused** — threads are not created/destroyed per operation; they are borrowed and returned.

#### Does it have a global limit?

**No global cap exists across concurrent operations.** If you run `K` concurrent images with `vips_concurrency_set(N)`, you can have up to `K × N` threads simultaneously:

```
16 goroutines × vips_concurrency=4 → up to 64 worker threads
+ 16 calling threads
= 80 threads on 8 CPU cores → massive context switching, no benefit
```

This is exactly why imgproxy sets `vips_concurrency_set(1)`. With concurrency=1, no worker threads are spawned — the operation runs entirely on the calling thread. The Go semaphore (`processingSem`, size=`Workers`) becomes the **sole** concurrency knob:

```
Workers=16, concurrency=1 → exactly 16 OS threads doing vips work  ✓
Workers=16, concurrency=4 → up to 80 threads competing on N cores  ✗
```

#### For C++ port

Same rule applies. Keep `vips_concurrency_set(1)` and control parallelism exclusively through your vips thread pool size.

### 1.4 Sequential Streaming Pipeline

Every loader passes `"access", VIPS_ACCESS_SEQUENTIAL`. This tells libvips to process image data as a lazy row-streaming pipeline — pixels are never fully decoded into RAM unless forced.

The pipeline is *forced* to random-access (materialised) only when required:

```go
// These operations require random access — must call CopyMemory first:
// processing/rotate_and_flip.go:18
// processing/apply_filters.go:14-16
// processing/watermark.go:67-72
// processing/crop.go:24-27 (smartcrop)
// processing/trim.go:35-37
img.CopyMemory()   // → C.vips_image_copy_memory
```

`vips_image_copy_memory` materialises all pending lazy operations before passing to rotate/smartcrop/watermark etc. The explicit `LineCache` wrapper (`vips/vips.c:895-900`) provides bounded-lookahead streaming:

```c
vips_linecache(in, out,
    "tile_height", tile_height,
    "access", VIPS_ACCESS_SEQUENTIAL,
    NULL);
```

### 1.5 Image Loading (Buffer-Based)

imgproxy **never uses file paths with libvips**. All loading is `*load_buffer` variants operating on downloaded bytes:

| Format | C function | Notes |
|--------|-----------|-------|
| JPEG | `vips_jpegload_buffer` | `shrink` param: free DCT pre-downscale (1/2/4/8) |
| JPEG-XL | `vips_jxlload_buffer` | |
| PNG | `vips_pngload_buffer` | |
| WebP | `vips_webpload_buffer` | |
| GIF | `vips_gifload_buffer` | |
| SVG | `vips_svgload_buffer` | |
| HEIC/AVIF | `vips_heifload_buffer` | `thumbnail=TRUE` for embedded thumbs |
| TIFF | `vips_tiffload_buffer` | complex fixup chain for float/BW TIFFs |
| ICO | pure Go (`vips/ico.go`) | decode → PNG → pass to vips |
| BMP | pure Go (`vips/bmp.go`) | |

Load entry point (`vips/vips.go:372-393`):
```go
C.vips_jpegload_go(
    unsafe.Pointer(&imgdata.Data[0]),
    C.size_t(len(imgdata.Data)),
    C.int(shrink),
    &img.VipsImage,
)
```

### 1.6 Resize

`vips_resize_go` at `vips/vips.c:344-364`:

```c
// Fast path: no alpha
vips_resize(in, out, wscale, "vscale", hscale, NULL);

// Alpha path: premultiply → cast(float) → resize → unpremultiply → cast(original)
vips_premultiply(in, &t[0], NULL);
vips_cast(t[0], &t[1], VIPS_FORMAT_FLOAT, NULL);
vips_resize(t[1], &t[2], wscale, "vscale", hscale, NULL);
vips_unpremultiply(t[2], &t[3], NULL);
vips_cast(t[3], out, fmt, NULL);
```

Alpha premultiplication prevents darkening of transparent edges during resampling. All temporaries use `vips_object_local_array(base, N)` — freed when `base` is unreffed.

### 1.7 Crop

| Operation | C function | Notes |
|-----------|-----------|-------|
| Rectangle | `vips_extract_area` | Exact pixel crop |
| Content-aware | `vips_smartcrop` | Finds salient region; requires `CopyMemory` first |
| Whitespace trim | `vips_trim` (custom) | `vips_find_trim` + `vips_extract_area` + symmetric mode; requires `CopyMemory` |

### 1.8 Format Conversion / Save

All saves are `*save_buffer` variants. Output is a GLib-malloc'd `void*` returned to Go as a zero-copy unsafe slice.

| Format | C function | Key options |
|--------|-----------|------------|
| JPEG | `vips_jpegsave_buffer` | Q, `optimize_coding=TRUE`, interlace |
| JPEG-XL | `vips_jxlsave_buffer` | Q, effort |
| PNG | `vips_pngsave_buffer` | palette quantize; `filter=ALL` (non-palette) / `NONE` (palette) |
| WebP | `vips_webpsave_buffer` | Q, effort, preset (photo/picture/drawing/icon/text) |
| GIF | `vips_gifsave_buffer` | auto bit-depth |
| HEIC | `vips_heifsave_buffer` | `HEVC` compression |
| AVIF | `vips_heifsave_buffer` | `AV1` compression, `effort = 9 - speed` |
| TIFF | `vips_tiffsave_buffer` | Q |

If `MaxBytes` is configured, imgproxy halves quality and re-saves in a loop (`processing/processing.go:218-248`), calling `vips_image_copy_memory` before each attempt to prevent re-running the full pipeline.

### 1.9 Filters, Composition, Color

**Filters** (`vips_apply_filters`, `vips/vips.c:591-688`):
- Gaussian blur: `vips_gaussblur`
- Sharpening: `vips_sharpen`
- Pixelate: `vips_shrink` + `vips_zoom`
- Alpha images: `vips_premultiply` / `vips_unpremultiply` wrap every filter

**Watermark** (`vips_apply_watermark`, `vips/vips.c:843-893`):
- `vips_composite2` with `VIPS_BLEND_MODE_OVER`
- Tiling via `vips_replicate` + `vips_extract_area`
- Positioning via `vips_embed`

**Color / ICC** (`vips/vips.c:429-565`):
- `vips_icc_import` / `vips_icc_export` / `vips_icc_transform`
- Target space: `VIPS_INTERPRETATION_scRGB` (linear) or `VIPS_INTERPRETATION_sRGB`
- Custom ICC backup/restore via `imgproxy-icc-profile` metadata key

**Geometric transforms**:
- `vips_rot` (0/90/180/270), `vips_flip` (H/V), `vips_embed` (padding)

**Animation** (`processing/processing.go:100-190`):
- Each frame: `vips_extract_area` → full 15-step pipeline → `vips_arrayjoin` (stack vertically)
- Page height metadata preserved so libvips re-interprets as animation on save

### 1.10 VipsImage Lifecycle Management

Core pattern in Go (`vips/vips.go:334-339`):

```go
func (img *Image) swapAndUnref(newImg *C.VipsImage) {
    if img.VipsImage != nil {
        C.unref_image(img.VipsImage)  // → VIPS_UNREF(in)
    }
    img.VipsImage = newImg
}
```

Every operation unrefs the previous `VipsImage` immediately via `swapAndUnref`.

In C bridge functions, all intermediates use `vips_object_local_array(VIPS_OBJECT(base), N)` — freed by a single `VIPS_UNREF(base)` at end of function, even on error paths.

Save output buffer lifecycle:
```go
// Zero-copy slice over GLib-malloc'd buffer
(*[math.MaxInt32]byte)(ptr)[:size:size]
// Freed via: ImageData.Close() → g_free_go() → g_free(*buf)
```

### 1.11 Full 15-Step Processing Pipeline

Defined at `processing/processing.go:21-37`:

```
1.  trim              → vips_trim (whitespace removal, if configured)
2.  prepare           → colourspace detection, animation split, DPR scale
3.  scaleOnLoad       → JPEG shrink factor pre-pass + initial thumbnail scale
4.  colorspaceToProcessing → vips_icc_import / vips_colourspace → scRGB/sRGB
5.  crop              → vips_smartcrop or vips_extract_area
6.  scale             → vips_resize (with alpha premultiply path)
7.  rotateAndFlip     → vips_rot, vips_autorot, vips_flip + CopyMemory
8.  cropToResult      → vips_extract_area (final dimensions)
9.  applyFilters      → blur, sharpen, pixelate + CopyMemory
10. extend            → vips_embed (padding)
11. extendAspectRatio → vips_embed (aspect ratio padding)
12. padding           → vips_embed (explicit padding values)
13. fixSize           → vips_extract_area (ensure exact pixel dimensions)
14. flatten           → vips_flatten (alpha → background colour)
15. watermark         → vips_composite2 + CopyMemory
```

After pipeline: `finalizePipeline` (colorspaceToResult + stripMetadata), then `img.Save(format, quality)`.
Between each step: `router.CheckTimeout(ctx)` aborts if request context is done.

---

## 2. Concurrency Model

### 2.1 HTTP Server & Connection Limiting

```
net/http Server
  └─ reuseport.Listen (SO_REUSEPORT)
  └─ netutil.LimitListener(l, MaxClients=2048)   ← hard TCP cap
```

`net/http` spawns one goroutine per accepted connection. `LimitListener` blocks `Accept()` when `MaxClients` is reached, applying TCP-level backpressure.

### 2.2 Two-Tier Admission Control

```
Incoming request
    │
    ▼
queueSem.TryAcquire(1)      ← size = Workers + RequestsQueueSize
    │ fail → HTTP 429
    │ ok
    ▼
processingSem.Acquire(ctx)  ← size = Workers (= GOMAXPROCS * 2)
    │ blocks until slot free (or ctx timeout)
    │ ok
    ▼
[ Download + vips processing ]
```

- `Workers` default: `runtime.GOMAXPROCS(0) * 2` (`config/config.go:242`), env: `IMGPROXY_WORKERS`.
- `RequestsQueueSize` default: 0, env: `IMGPROXY_REQUESTS_QUEUE_SIZE`.
- The download runs **inside** `processingSem` by design — image buffers are sized to `Workers`.

### 2.3 S3 Download Architecture

S3 is plugged in as an `http.RoundTripper` registered for `s3://`:

```
imagedata.Download(ctx, "s3://bucket/key")
    → http.Client.Do(req)
    → s3.Transport.RoundTrip(req)
        → aws-sdk-go-v2: client.GetObject(ctx, input)
        → returns output.Body directly as http.Response.Body (streamed)
```

Key details:
- **Direct `GetObject`**, not presigned URLs.
- `Range`, `If-None-Match`, `If-Modified-Since` forwarded to S3.
- Auto-retry with region-corrected client on 301 redirect.
- Per-region/per-bucket client cache via `sync.RWMutex`.
- Context threaded to `GetObject` — cancellation aborts the download.

HTTP transport tuning (`transport/transport.go:16-65`):
```go
MaxIdleConns:          100
MaxIdleConnsPerHost:   config.Workers + 1
HTTP2:                 enabled (MaxReceiveBufferPerStream=128KB)
DisableCompression:    true   // imgproxy handles gzip itself
DialContext:           SSRF protection via verifySourceNetwork
```

### 2.4 Why imgproxy Uses LockOSThread Per Request

This is the most important concurrency detail for understanding the C++ port.

#### Go's M:N scheduler: goroutines migrate between OS threads

Go multiplexes many goroutines (`G`) onto a smaller pool of OS threads (`M`). The scheduler is preemptive and opaque — it can migrate a goroutine to a different OS thread at any safepoint (channel ops, function calls, CGo returns):

```
Goroutine A on OS thread M1:
  C.vips_resize_go(...)     ← starts, libvips sets up TLS on M1
  [CGo returns to Go]
  [Go scheduler fires — migrates Goroutine A to M2]
  C.vips_image_copy_memory  ← runs on M2, vips TLS is on M1  ← WRONG!
  defer vips.Cleanup()      ← calls vips_thread_shutdown() on M2
                              → cleans M2's TLS
                              → M1's TLS is now leaked forever
```

#### What libvips stores per OS thread (TLS)

| TLS item | What it is |
|----------|-----------|
| Error buffer | String accumulator for `vips_error_buffer()` — each thread has its own |
| GLib thread data | GLib's own `GPrivate`/`GStaticPrivate` slots |
| Slab allocator state | Per-thread memory slab (fast alloc for small objects) |
| Thread operation context | vips tracks which thread is running which operation |

#### Failure modes without LockOSThread

- **Wrong error string** — error from another thread appears in your response
- **TLS leak** — memory from uncleaned threads accumulates unbounded
- **SIGSEGV** — if vips checks thread identity for an operation
- **vips_thread_shutdown on wrong thread** — leaves the actual working thread's state dirty

#### The fix

```go
// processing/processing.go:250-254
func ProcessImage(...) {
    runtime.LockOSThread()      // pin goroutine to current OS thread — no migration
    defer runtime.UnlockOSThread()
    defer vips.Cleanup()        // → vips_thread_shutdown() on THIS thread
    ...
}
```

`runtime.LockOSThread()` makes the goroutine and OS thread 1:1 for the duration. When done:
1. `vips_thread_shutdown()` cleans libvips TLS on the correct thread
2. `UnlockOSThread()` releases the thread back to Go's scheduler
3. The next goroutine on this thread gets clean vips TLS state

#### The per-request cost

Every image processing request pays:
- `LockOSThread` system call
- `vips_thread_shutdown()` CGo call (iterates GLib private cleanup handlers) — teardown and re-init per request
- `UnlockOSThread`

This cost does **not exist** in the C++ design. See [section 5.3](#53-go-goroutines-vs-c-coroutines-the-tls-problem).

### 2.5 Request Pipeline End-to-End

```
TCP accept (blocked by LimitListener at MaxClients)
    │
    ▼
net/http goroutine (one per connection)
    ├─ Parse URL, verify HMAC signature
    ├─ Parse processing options
    │
    ├─ queueSem.TryAcquire(1) ──► 429 if full
    ├─ context.WithTimeout(Timeout)
    │
    ├─ processingSem.Acquire(ctx) ──► blocks until worker slot
    │   │
    │   ├─ context.WithTimeout(DownloadTimeout)
    │   ├─ imagedata.Download(ctx, url)
    │   │   └─ RoundTripper → HTTP/S3/GCS/Azure/FS → stream into bufpool buffer
    │   │
    │   ├─ router.CheckTimeout(ctx)
    │   │
    │   ├─ runtime.LockOSThread()
    │   ├─ processing.ProcessImage(ctx, imgdata, opts)
    │   │   ├─ vips load (buffer-based)
    │   │   ├─ 15-step pipeline (each step: CheckTimeout)
    │   │   └─ vips save → ImageData with g_free cancel
    │   ├─ runtime.UnlockOSThread()
    │   │
    │   └─ respondWithImage → write headers + body
    │
    ├─ processingSem.Release(1)
    └─ queueSem.Release(1)
```

### 2.6 Buffer Pooling

| Pool | Purpose | Size |
|------|---------|------|
| `downloadBufPool` | Downloaded image bytes | Calibrated to `Workers` |
| `streamBufPool` | Raw streaming 4KB copy buffer | `sync.Pool` |

`downloadBufPool` is custom (`bufpool/bufpool.go`): size-bucketed, `maxLen` keyed to `config.Workers`, guarded by `sync.Mutex`. The buffer's `Bytes()` is passed directly (no copy) to libvips loaders via `unsafe.Pointer`.

### 2.7 Context Propagation & Cancellation

```
context.WithTimeout(config.Timeout)           // per-request total
    └─ context.WithTimeout(DownloadTimeout)   // download subtimeout

Check points:
  processingSem.Acquire(ctx, 1)   // cancellable wait
  GetObject(ctx, ...)              // S3 download cancellable
  router.CheckTimeout(ctx)         // after download, between each pipeline step
  saveImageToFitBytes loop         // each quality iteration
```

Timeout → HTTP 503. Client disconnect → HTTP 499.

---

## 3. Memory Allocation: jemalloc & tcmalloc

### 3.1 imgproxy's Approach

**Default: glibc `malloc`** (`ENV IMGPROXY_MALLOC="malloc"` in the Dockerfile — the entrypoint no-ops this).

imgproxy ships both jemalloc and tcmalloc in its Docker image as **opt-in runtime alternatives**, selected via `IMGPROXY_MALLOC` env var. The `docker/entrypoint.sh` injects the chosen allocator via `LD_PRELOAD`:

```bash
# docker/entrypoint.sh
case "$IMGPROXY_MALLOC" in
  malloc)   # do nothing — glibc default
  jemalloc) export LD_PRELOAD="$LD_PRELOAD:/usr/local/lib/libjemalloc.so"
  tcmalloc) export LD_PRELOAD="$LD_PRELOAD:/usr/local/lib/libtcmalloc_minimal.so"
esac
exec "$@"
```

This is a **process-level symbol substitution** — replaces `malloc`/`free` for the whole process: libvips, Go runtime, GLib. No direct jemalloc API usage; just intercepting standard allocator symbols.

The `docker/Dockerfile` also sets `ENV MALLOC_ARENA_MAX=2` to limit glibc arena count and reduce fragmentation in the default case.

**Why the `malloc_trim` workaround exists** (`memory/free_linux.go:16-20`): glibc's allocator fragments heavily under libvips' allocation patterns (many small allocs for pixel tiles, rapid free/alloc cycles). `malloc_trim(0)` forces glibc to release free pages back to the OS. With jemalloc or tcmalloc (arena-per-thread design), this fragmentation does not occur and `malloc_trim` becomes a no-op.

### 3.2 AWS SDK C++ jemalloc Support

The AWS SDK for C++ supports jemalloc in two ways:

#### Option A: Global symbol replacement (simplest)

Link jemalloc before glibc so it intercepts all `malloc`/`free` symbols globally — the SDK uses it transparently:

```cmake
target_link_libraries(your_app jemalloc aws-cpp-sdk-s3 ...)
```

Or at runtime: `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./your_app`

#### Option B: AWS SDK custom memory manager (explicit, SDK-native)

The SDK has a first-class allocator interface:

```cpp
#include <jemalloc/jemalloc.h>
#include <aws/core/utils/memory/MemorySystemInterface.h>

class JemallocMemorySystem : public Aws::Utils::Memory::MemorySystemInterface {
public:
    void* AllocateMemory(std::size_t blockSize, std::size_t alignment,
                         const char* /*tag*/) override {
        return je_aligned_alloc(alignment, blockSize);
    }
    void FreeMemory(void* ptr) override {
        je_free(ptr);
    }
    void Begin() override {}
    void End() override {}
};

// At startup, before any SDK use:
JemallocMemorySystem mem;
Aws::SDKOptions options;
options.memoryManagementOptions.memoryManager = &mem;
Aws::InitAPI(options);
```

This routes **all** SDK internal allocations through jemalloc explicitly, regardless of link order.

**Recommendation**: Use **Option B** (explicit SDK memory manager) combined with global jemalloc linkage (`-ljemalloc`). The SDK's own allocations route through jemalloc explicitly, and libvips' `g_malloc` (GLib → glibc) also resolves to jemalloc via symbol interception. This eliminates both fragmentation vectors and makes the `malloc_trim` workaround unnecessary.

---

## 4. CGo Overhead Analysis

### 4.1 Call Frequency Per Request

A typical single-frame JPEG→WebP request makes approximately **30–60 CGo transitions**:

```
Load phase (~5 calls):
  vips_jpegload_go
  vips_image_get_typeof × 3   (metadata probes in prepare step)
  vips_image_get_int × 2

Resize (~4 calls):
  vips_resize_go
  + 3 extra if alpha (premultiply, cast, unpremultiply, cast)

Color (~6 calls):
  vips_colourspace_go × 2     (to processing + to result)
  vips_icc_import_go / vips_icc_export_go
  vips_image_guess_interpretation

Crop + pipeline (~5 calls):
  vips_extract_area_go
  vips_image_copy_memory

Metadata (~15-20 calls):
  vips_image_get_typeof × many  (per-step checks)
  vips_image_set_int × 4

Save (~3 calls):
  vips_image_copy_memory
  vips_webpsave_go
  vips_cleanup
```

For animated GIFs: multiply by frame count (each frame runs the full pipeline).

Each CGo transition costs ~200–300 ns. At 50 transitions: ~10–15 µs overhead — negligible compared to 50–500 ms vips processing cost. The CGo overhead is **not the bottleneck**.

### 4.2 Memory Passing Patterns

**Go → C (inputs):**

```go
// Image bytes: direct pointer, no copy
unsafe.Pointer(&imgdata.Data[0])   // slice over bufpool buffer

// Strings: interned C strings via sync.Map cache (vips/cached_c_strings.go)
// C.CString allocated once per unique attribute name, never freed
// Avoids malloc+memcpy on every metadata call
```

**C → Go (outputs):**

```go
// Image output: ZERO-COPY — unsafe slice over GLib-malloc'd buffer
(*[math.MaxInt32]byte)(ptr)[:size:size]
// Freed via: ImageData.Close() → g_free_go() → g_free(*buf)

// ICC blob: copies (C.GoBytes — safe path)
// Int arrays: unsafe cast then copy into []int
```

---

## 5. C++ Port: Performance Analysis & Design Guide

### 5.1 Where CGo Costs Live (and Don't)

The CGo overhead in imgproxy is not the bottleneck. Real costs that matter:

| Cost | Go impl | C++ potential gain |
|------|---------|-------------------|
| GC pauses | STW GC, 1–5ms p99, up to 20ms under pressure | Zero — no GC |
| Per-request thread lock/unlock | `LockOSThread` + `vips_thread_shutdown` every request | Thread pool: init TLS once per thread lifetime |
| CGo transitions | ~300 ns × 50 = 15 µs/request | Direct call: ~1 ns × 50 = 0.05 µs |
| Go→C memory pinning | `runtime.KeepAlive`, `unsafe` casts | Direct pointer, no pinning |
| Goroutine scheduler | M:N scheduler overhead, stack growth | OS threads + coroutines, deterministic |
| malloc fragmentation | `malloc_trim` periodic workaround | jemalloc linked directly: no fragmentation |

**Verdict**: Port is worth it primarily for tail latency (GC pauses eliminated), lower per-request overhead (no TLS teardown), and better memory control. Do not expect a 10× throughput gain — libvips CPU cost dominates. Expect **10–30% better throughput** and **significantly better p99/p999 latency**.

### 5.2 Real Performance Wins in C++

**1. Eliminate GC pauses**
```
Go:  STW GC pause 1–5ms p99, up to 20ms under memory pressure
C++: Zero pauses — use jemalloc to avoid fragmentation
```

**2. Eliminate per-request vips TLS teardown**
```
Go:  LockOSThread + vips_thread_shutdown() per request → re-init on next request
C++: Thread pool — vips TLS initialised once per thread, stays warm forever
```

**3. Eliminate CGo overhead**
```
Go:  ~300 ns per CGo call × 50 calls = 15 µs/request
C++: direct function call ~1 ns × 50 = 0.05 µs/request
```

**4. Zero-copy I/O pipeline**
```
Drogon async I/O → buffer → libvips load_buffer → save_buffer → response
All in one coroutine, no goroutine scheduler involvement
```

### 5.3 Go Goroutines vs C++ Coroutines: The TLS Problem

This is the most important design constraint for the C++ port.

#### Go goroutines: M:N scheduler with preemptive migration

The Go runtime has its own M:N scheduler. Goroutines can be preempted and moved to a different OS thread at safepoints **without your knowledge**:

```
Goroutine A running on OS thread M1
  → calls vips_resize (CGo)
  → CGo returns to Go
  → Go scheduler: "M2 is idle, move Goroutine A there"
  → Goroutine A now runs on M2
  → vips TLS is still on M1  ← PROBLEM
```

You cannot control which OS thread a goroutine runs on without `LockOSThread`. This forces the per-request TLS teardown pattern.

#### C++ coroutines: stackless, no built-in scheduler

C++ coroutines (`co_await`, `co_return`) are **stackless state machines** compiled into a heap-allocated frame. The language provides the mechanism; **there is no built-in scheduler**. Which thread resumes a coroutine is **entirely determined by the awaitable you write**:

```cpp
co_await some_operation();
//        ↑
//   This object decides WHEN and ON WHICH THREAD to resume the coroutine.
//   The language says nothing about it.
```

**C++ coroutines do not migrate themselves** — they resume wherever the executor places them.

#### Three scenarios

**Scenario 1: Drogon I/O coroutines — safe by default**

Drogon's event loop is single-threaded per loop. `co_await` on a network operation resumes on the same event loop thread:

```
Event loop thread 1:
  coroutine starts
  co_await http_download(url)      ← suspends, frame stored on heap
  [I/O completes, Drogon resumes on SAME event loop thread 1]
  coroutine continues on thread 1  ← same thread, TLS consistent ✓
```

**Scenario 2: co_await into vips thread pool — safe if designed correctly**

```cpp
auto result = co_await vips_pool.submit([]{
    // Runs SYNCHRONOUSLY on vips thread 5
    // No co_await inside — straight through, one thread
    auto img = vips_load(...);
    auto out = run_pipeline(img, opts);
    return vips_save(out, fmt);
    // TLS: init on thread 5, used on thread 5, stays on thread 5 ✓
});
// Coroutine resumes wherever submit's awaitable decides — fine, vips is already done
```

The vips lambda runs entirely on one thread with no suspension — TLS is consistent. ✓

**Scenario 3: DANGEROUS — co_await inside vips pipeline**

```cpp
// DO NOT DO THIS:
VipsImage* img = vips_jpegload_buffer(...);    // thread 5, TLS init on 5
auto watermark = co_await download_watermark(); // ← SUSPEND
                                                // may resume on thread 3!
vips_composite2(img, watermark, ...);           // thread 3, TLS on 5 ← SAME PROBLEM AS GO
```

If you `co_await` in the middle of a vips pipeline and the awaitable resumes on a different thread, you get the **exact same TLS corruption as imgproxy's goroutine problem**.

### 5.4 The Rule: Never co_await Inside a vips Pipeline

```
┌──────────────────────────────────────────────────────────────────┐
│  NEVER co_await inside a vips pipeline.                          │
│  Run the entire vips pipeline as one synchronous function        │
│  on a dedicated vips thread. co_await only the final result.     │
└──────────────────────────────────────────────────────────────────┘
```

All I/O (downloads) must be completed **before** entering the vips pipeline:

```cpp
// ✓ Correct design
drogon::Task<HttpResponsePtr> handle(HttpRequestPtr req) {
    // I/O phase — all co_await allowed here, on event loop thread
    auto img_data   = co_await download_image(req->imageUrl());
    auto watermark  = co_await download_watermark(req->watermarkUrl());

    // vips phase — synchronous, one thread, NO co_await inside
    auto result = co_await vips_pool.submit([&]{
        auto img = vips_load(img_data);
        auto wm  = vips_load(watermark);
        return run_pipeline_sync(img, wm, opts);  // all 15 steps, synchronous
    });

    co_return build_response(result);
}
```

### 5.5 Drogon Architecture Mapping

| imgproxy (Go) | C++ Drogon equivalent |
|--------------|----------------------|
| `net/http` + goroutine per conn | Drogon event loop (epoll/kqueue) |
| `processingSem` (Workers slots) | `VipsThreadPool` size = Workers |
| `queueSem` (queue + workers) | Drogon request queue + semaphore |
| `LimitListener(MaxClients)` | `app().setMaxConnectionNum()` |
| `context.WithTimeout` | `trantor::EventLoop::runAfter` + cancel token |
| `router.CheckTimeout` per step | Cancellation token checked between pipeline steps |
| `runtime.LockOSThread` + `vips_thread_shutdown` per request | vips thread pool: TLS init once per thread |

**Critical**: Drogon's I/O threads must **not** run vips directly. Keep I/O and CPU in separate thread pools, connected by coroutines.

```cpp
// Thread allocation:
app().setThreadNum(numIoThreads);    // Drogon I/O event loops
VipsThreadPool vips_pool{workers};   // Separate, dedicated vips threads
```

### 5.6 libvips C++ Integration Design

#### Thread initialization (once per thread, not per request)

```cpp
// Called once when a vips worker thread starts:
void vips_worker_thread_init() {
    vips_concurrency_set(1);
    // TLS is now initialised for this thread — stays warm for all requests
}

// Called once when a vips worker thread exits (pool shutdown):
void vips_worker_thread_exit() {
    vips_thread_shutdown();
}
```

This replaces imgproxy's per-request `LockOSThread` + `vips_thread_shutdown` with a one-time cost per thread lifetime.

#### RAII VipsImage wrapper

```cpp
class VipsImageGuard {
    VipsImage* img_ = nullptr;
public:
    explicit VipsImageGuard(VipsImage* img) : img_(img) {}
    ~VipsImageGuard() { VIPS_UNREF(img_); }

    VipsImage* get() const { return img_; }
    VipsImage* release() { return std::exchange(img_, nullptr); }

    VipsImageGuard(const VipsImageGuard&) = delete;
    VipsImageGuard(VipsImageGuard&& o) noexcept
        : img_(std::exchange(o.img_, nullptr)) {}
};
```

#### Pipeline with safe intermediate management

```cpp
// Mirrors vips_object_local_array: all intermediates freed via RAII
VipsImage* resize_with_alpha(VipsImage* in, double wscale, double hscale) {
    VipsImage* t[5] = {};
    auto cleanup = [&]{ for (auto p : t) VIPS_UNREF(p); };

    int fmt = vips_image_get_format(in);
    if (vips_premultiply(in, &t[0], nullptr) ||
        vips_cast(t[0], &t[1], VIPS_FORMAT_FLOAT, nullptr) ||
        vips_resize(t[1], &t[2], wscale, "vscale", hscale, nullptr) ||
        vips_unpremultiply(t[2], &t[3], nullptr) ||
        vips_cast(t[3], &t[4], (VipsBandFormat)fmt, nullptr)) {
        cleanup();
        return nullptr;  // vips_error_get_all() for details
    }
    auto result = t[4]; t[4] = nullptr;
    cleanup();
    return result;
}
```

#### Buffer-based load and save

```cpp
// Load — matching imgproxy's VIPS_ACCESS_SEQUENTIAL approach
VipsImage* load_jpeg(const uint8_t* data, size_t len, int shrink) {
    VipsImage* out = nullptr;
    vips_jpegload_buffer(
        const_cast<void*>(static_cast<const void*>(data)), len,
        &out,
        "access", VIPS_ACCESS_SEQUENTIAL,
        "shrink", shrink,
        nullptr
    );
    return out;
}

// Save — output buffer owned by GLib, freed on response send
struct VipsBuffer {
    void* data = nullptr;
    size_t size = 0;
    ~VipsBuffer() { if (data) g_free(data); }
    // Move-only
    VipsBuffer(VipsBuffer&& o) noexcept
        : data(std::exchange(o.data, nullptr)), size(o.size) {}
};

VipsBuffer save_webp(VipsImage* img, int quality, int effort) {
    VipsBuffer buf;
    vips_webpsave_buffer(img, &buf.data, &buf.size,
        "Q", quality, "effort", effort, nullptr);
    return buf;
}
```

#### Materialize for random-access operations

```cpp
// Mirror imgproxy's CopyMemory pattern — call before smartcrop/rotate/watermark/trim
VipsImageGuard materialize(VipsImage* in) {
    VipsImage* out = nullptr;
    vips_image_copy_memory(in, &out);
    return VipsImageGuard{out};
}
```

#### Global initialization

```cpp
void vips_init_imgproxy() {
    vips_init("imgproxy-cpp");
    vips_cache_set_max_mem(0);  // disable cache — same reasoning as Go
    vips_cache_set_max(0);
    vips_concurrency_set(1);    // per-op single-threaded; pool provides parallelism
}
```

### 5.7 S3 Download in C++

imgproxy uses direct `GetObject` (not presigned URLs), streamed, with context cancellation. Three options for C++:

**Option A: AWS SDK for C++ — closest to imgproxy (recommended for production)**
```cpp
auto outcome = co_await async_s3_get_object(client, bucket, key, range_header);
if (!outcome.IsSuccess()) co_return error_response(outcome.GetError());
auto& body = outcome.GetResult().GetBody();
// stream body into buffer
```

Regional client caching:
```cpp
std::shared_mutex clients_mu;
std::unordered_map<std::string, Aws::S3::S3Client> clients_by_region;
// Read lock for lookups, write lock for new regions — same as imgproxy's sync.RWMutex
```

**Option B: Presigned URL + Drogon HTTP client (fastest initial port)**
```cpp
// Generate presigned URL (no network call)
auto url = presigner.PresignGetObject(req, std::chrono::minutes(1));
// Fetch via Drogon's co_await-friendly HTTP client
auto resp = co_await drogon::HttpClient::newHttpClient(url)
                ->sendCoroutine(drogon::HttpRequest::newHttpRequest());
```

**Option C: libcurl async with Drogon event loop**
```cpp
// Use curl multi interface integrated with Drogon's event loop
// Requires SigV4 signing (use aws-crt-cpp or implement manually)
```

For jemalloc + AWS SDK: use **Option B from section 3.2** (explicit custom memory manager) plus global `-ljemalloc` linkage.

### 5.8 C++ Coroutine Concurrency Model

The full pipeline mirrors imgproxy's semaphore-gated model:

```cpp
// Global admission control (init at startup)
std::counting_semaphore<> queue_sem{workers + queue_size};
// VipsThreadPool implicitly limits to Workers concurrent vips ops

drogon::Task<HttpResponsePtr> handle_request(HttpRequestPtr req) {
    // Tier 1: fast reject (non-blocking)
    if (!queue_sem.try_acquire())
        co_return make_response(drogon::k429TooManyRequests);
    auto queue_guard = scope_exit([&]{ queue_sem.release(); });

    // Parse options, verify HMAC signature
    auto opts = parse_options(req);

    // I/O phase — all co_await here is fine (Drogon event loop, same thread)
    auto img_data  = co_await download_image(opts.url);
    auto watermark = opts.watermark ? co_await download_watermark(opts.watermark_url)
                                    : ImageData{};

    // vips phase — submitted to dedicated thread pool
    // Tier 2 limiting: VipsThreadPool has fixed Workers slots (blocks on submit)
    auto result = co_await vips_pool.submit([img_data, watermark, opts]{
        // Entire synchronous pipeline — no co_await, one thread
        return process_image_sync(img_data, watermark, opts);
    });

    co_return build_response(result);
}
```

**VipsThreadPool design** (each thread lives forever, TLS warm):

```cpp
class VipsThreadPool {
    std::vector<std::thread> threads_;
    moodycamel::ConcurrentQueue<std::packaged_task<void()>> queue_;
    std::counting_semaphore<> slots_;  // limits concurrency = Workers

public:
    explicit VipsThreadPool(int workers) : slots_{workers} {
        for (int i = 0; i < workers; ++i) {
            threads_.emplace_back([this]{
                vips_worker_thread_init();   // TLS init ONCE
                work_loop();
                vips_worker_thread_exit();   // TLS cleanup ONCE
            });
        }
    }

    template<typename F>
    drogon::Task<std::invoke_result_t<F>> submit(F&& f) {
        slots_.acquire();                  // blocks coroutine (Tier 2 gate)
        auto guard = scope_exit([this]{ slots_.release(); });
        // package, push, co_await future
        ...
    }
};
```

### 5.9 Memory Management Strategy

**Replace Go's bufpool with per-thread or lock-free alternatives:**

```cpp
// Option 1: thread_local buffer (zero-allocation on reuse per vips thread)
thread_local std::vector<uint8_t> download_buffer;

// Option 2: Lock-free pool (matches bufpool semantics across threads)
template<int MaxItems>
class BufferPool {
    std::array<std::unique_ptr<std::vector<uint8_t>>, MaxItems> pool_;
    moodycamel::ConcurrentQueue<std::vector<uint8_t>*> free_list_;
public:
    std::vector<uint8_t>* acquire(size_t reserve_size);
    void release(std::vector<uint8_t>*);
};
```

**Replace glibc + `malloc_trim` with jemalloc:**

```cmake
# CMakeLists.txt
find_package(PkgConfig REQUIRED)
pkg_check_modules(JEMALLOC REQUIRED jemalloc)
target_link_libraries(imgproxy-cpp ${JEMALLOC_LIBRARIES} aws-cpp-sdk-s3 vips ...)
```

With jemalloc's arena-per-thread design, libvips' allocation pattern (many small tiles, rapid free/alloc cycles) does not fragment, and the `malloc_trim` workaround is unnecessary.

**GLib output buffer** (`g_free` on response send): acceptable as-is. If you want custom allocation for vips output, use `vips_malloc_array` with a custom allocator, but this is an advanced optimization.

### 5.10 Traps & Risks

| Risk | Detail | Mitigation |
|------|--------|-----------|
| vips TLS per-thread | `vips_thread_shutdown()` must be called when a thread exits, not per-request | Init pool once; call shutdown only on pool destruction |
| co_await inside vips pipeline | Suspending mid-pipeline resumes on a different thread → TLS corruption | Run entire pipeline as one synchronous function; no `co_await` inside |
| vips sequential access + CopyMemory | `VIPS_ACCESS_SEQUENTIAL` images are invalid for random-access ops | Call `vips_image_copy_memory` before smartcrop/rotate/watermark/trim — same as imgproxy |
| vips error handling | vips returns 0/non-zero; error string is TLS-local (`vips_error_buffer()`) | Always check return values; call `vips_error_clear()` after error; errors ARE thread-local in modern vips |
| Animation frame count | Multi-frame images need `n=-1` in loader to load all pages | Check `vips_image_get_n_pages()` and reload with `n=-1` if > 1 |
| ICC profile round-trip | imgproxy saves/restores ICC metadata across ops that strip it | Implement `backup_icc`/`restore_icc` via `vips_image_get_blob`/`vips_image_set_blob` |
| vips cache | C++ inherits default (enabled) — same SIGSEGV risk on musl | Call `vips_cache_set_max(0)` in init |
| S3 connection pooling | Need `MaxIdleConnsPerHost` equivalent | AWS SDK C++ client pool: set `maxConnections = workers + 1` in `ClientConfiguration` |
| Drogon thread ≠ vips thread | Never run vips on Drogon's I/O event loop | `app().setThreadNum(ioThreads)` is separate from `VipsThreadPool(workers)` |
| Coroutine resume thread | If vips pool's awaitable resumes on a random thread, subsequent vips calls on that thread have cold TLS | Ensure `process_image_sync` is fully self-contained; no vips calls after the `co_await` on the event loop thread |
| jemalloc + AWS SDK | SDK may still use glibc malloc if linked after | Use SDK custom memory manager (Option B) + `-ljemalloc` to guarantee both are covered |

---

## 6. Summary Comparison Table

| Aspect | imgproxy (Go) | C++ + Drogon target |
|--------|--------------|---------------------|
| vips call method | CGo bridge (~300 ns/call) | Direct C call (~1 ns/call) |
| vips pipeline | Sequential (`VIPS_ACCESS_SEQUENTIAL`) | Same — no change |
| vips concurrency | 1 thread/op; Go goroutines outside | 1 thread/op; vips thread pool outside |
| vips cache | Disabled | Disabled |
| vips thread init | `LockOSThread` + `vips_thread_shutdown` **per request** | Init once per thread lifetime |
| HTTP server | `net/http` + goroutine-per-conn | Drogon event loop (epoll) |
| I/O model | goroutines (M:N, preemptive migration) | coroutines on event loop (deterministic thread) |
| TLS problem | Always present — requires `LockOSThread` | Only if `co_await` inside vips — avoid by design |
| S3 download | AWS SDK Go v2, `GetObject`, streamed | AWS SDK C++, `GetObject`, streamed |
| Admission control | Two semaphores (queue + processing) | Semaphore + VipsThreadPool size |
| Buffer pool | Custom `bufpool`, mutex-guarded | `thread_local` or lock-free ring |
| Output buffer | Zero-copy (`unsafe.Pointer` over `g_malloc`) | Zero-copy (raw pointer over `g_malloc`) |
| Memory allocator | glibc (default), jemalloc/tcmalloc opt-in via LD_PRELOAD | jemalloc linked directly (`-ljemalloc`) |
| GC | Go GC, 1–5ms STW pauses | None |
| malloc fragmentation | `malloc_trim` periodic goroutine | jemalloc eliminates fragmentation |
| Expected throughput gain | baseline | ~10–30% |
| Expected p99 latency gain | baseline | Significant (GC pauses + TLS overhead eliminated) |

---

*Document based on imgproxy master branch (`61c01ab7`). Key source files: `vips/vips.go`, `vips/vips.c`, `processing/processing.go`, `processing_handler.go`, `transport/s3/s3.go`, `server.go`, `config/config.go`, `docker/entrypoint.sh`, `memory/free_linux.go`.*
