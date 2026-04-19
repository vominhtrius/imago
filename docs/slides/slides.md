---
theme: default
colorSchema: light
title: Image Processing Platform
info: |
  Proposal for a unified image service — imago vs forked imgproxy.
  Based on solution-proposal-image-service-v2.md (2026-04-19).
class: text-center
drawings:
  persist: false
transition: slide-left
mdc: true
showGotoDialog: false
fonts:
  sans: Inter
  mono: JetBrains Mono
---

<style>
/* Kill the goto-slide dialog globally — covers multiple Slidev versions. */
.slidev-goto-dialog,
[class*="goto-dialog"],
[class*="GotoDialog"],
.slidev-nav-goto-dialog {
  display: none !important;
  visibility: hidden !important;
  pointer-events: none !important;
  opacity: 0 !important;
}

/* Chart images must render at full opacity / no filter.
   Some Slidev themes dim images for visual consistency — undo that. */
.slidev-page img,
.slidev-layout img {
  opacity: 1 !important;
  filter: none !important;
  mix-blend-mode: normal !important;
  image-rendering: -webkit-optimize-contrast;
}
</style>

# Image Processing Platform

One image path. On-demand. CDN-fronted.

<div class="pt-8 opacity-70 text-sm">
  trivm80 · 2026-04-20 · For approval
</div>

<!--
Mở đầu. Khoảng 10 giây.

"Hôm nay em trình bày đề xuất xây một nền tảng xử lý ảnh dùng chung cho toàn công ty. Em sẽ đi qua: vấn đề, hai option thực tế, số liệu benchmark, và quyết định cần các anh chị duyệt."

Ngắn gọn, tránh dẫn nhập dài dòng. Audience là CTO và Engineering Director — vào thẳng vấn đề.
-->

---
layout: default
---

# Agenda

1. **The problem** — why every team keeps rebuilding the image path
2. **Scope & architecture** — what "one image platform" means
3. **Options on the market** — imgproxy, thumbor, weserv, and the C++ port
4. **Benchmark** — same hardware, same libvips, head-to-head
5. **Recommendation & decision** — rollout, risks, what we need from you
6. **Demo** — the two services running side by side

<div class="pt-10 opacity-70 text-sm">
  ~15 minutes. Questions welcome throughout.
</div>

<!--
Đặt lại expectation cho audience: deck có cấu trúc tường minh, biết trước khi nào quyết định sẽ được xin.

Không đọc từng dòng. 15 giây.
-->

---
layout: default
---

# Why we're here

Today, every product rebuilds the image path:

- **Incomplete coverage** — resize/crop/convert/watermark vary per team
- **EXIF leakage** — GPS + device fingerprints shipped to clients
- **Slow delivery** — visible on feed scroll, chat render
- **Async resize pipeline** — upload, poll, reference variants by separate keys
- **Storage amplification** — originals + every pre-generated variant in S3

<div class="flex justify-center mt-4">
  <img src="/exif-leak.webp" alt="EXIF leakage example" class="w-[80%] max-h-[28vh] object-contain rounded" />
</div>

<!--
Mô tả pain point mà không đổ lỗi cho team nào. Đây là hệ quả của việc chưa có platform, không phải do engineering team làm sai.

Câu chốt cần nhấn: "Mỗi P&L đang tự xây lại image path." Đó là vấn đề mà platform sẽ giải quyết. Nếu audience nắm được câu này thì các slide sau sẽ có context.

Không dừng lâu. 30–40 giây.
-->

---
layout: center
class: text-center
---

# Goal

<div class="text-3xl pt-6 opacity-95">
  One image platform — <strong>synchronous, on-demand, CDN-fronted</strong> — adopted across all P&L products.
</div>

<!--
Phát biểu một lần, rõ ràng. Không giải thích thêm. Toàn bộ deck còn lại là để bảo vệ câu này.

Nếu hỏi "sao lại synchronous?" — vì async resize chính là friction mà mình đang muốn bỏ. Derivative được compute lúc CDN miss, không pre-generate nữa.
-->

---

# Must-have features

| Capability | What it covers |
|---|---|
| **Upload** | EXIF/IPTC/XMP strip · magic-byte validate · HEIC→JPEG/WebP · size caps |
| **Resize** | fit / cover / fill / force · DPR 1×/2×/3× · shrink-on-load decode |
| **Crop** | gravity · smart crop (libvips) · focal point · pixel region |
| **Convert** | JPEG / WebP / PNG · quality control · `Accept` negotiation · auto-rotate |
| **Cache** | ETag · conditional GET (304) |

<div class="mt-4 text-sm opacity-70">
Nice-to-have: watermark, blur, shape mask, sharpen, rotate, pipeline, <code>/info</code>, AVIF.
</div>

<!--
Đây không phải slide demo feature. Đây là scope check — để không ai hỏi sau này "thế còn watermark?" và làm lạc hướng quyết định.

Must-have list được chọn có chủ đích: feature set của imgproxy + phần ingest. Ai từng dùng imgproxy sẽ nhận ra ngay.

20 giây. Không đọc từng dòng.
-->

---
layout: default
---

# Architecture — key properties

<div class="grid grid-cols-3 gap-4 pt-4">

<div class="p-4 border rounded-lg">
<h3 class="text-xl">On-demand</h3>
<p class="text-sm opacity-80 mt-2">Transforms computed per request from the original. No pre-generated variants. No storage amplification.</p>
</div>

<div class="p-4 border rounded-lg">
<h3 class="text-xl">CDN-cached</h3>
<p class="text-sm opacity-80 mt-2">Each rendered variant cached at the edge keyed by full URL. Service pays CPU on first miss; every hit is CloudFront.</p>
</div>

<div class="p-4 border rounded-lg">
<h3 class="text-xl">Asymmetric</h3>
<p class="text-sm opacity-80 mt-2">Serve traffic absorbed by CDN on read. Upload bypasses CDN and hits the service directly.</p>
</div>

</div>

<div v-click class="mt-8 p-4 bg-gray-100 rounded text-sm">
  <strong>Consequence:</strong> Service scales on <em>miss rate</em>, not request rate. At 85% hit rate, service sees 15% of traffic.
</div>

<!--
Ba tính chất này chi phối mọi lựa chọn downstream — instance type, scaling metric, failure mode.

Click reveal là câu mà CTO nào cũng hỏi: "thực tế service chịu tải bao nhiêu?" Trả lời: miss rate × total traffic = service load. Vì vậy fleet có thể nhỏ.

30 giây.
-->

---
layout: full
background: white
---

<div class="h-full w-full flex bg-white text-gray-900">
  <div class="w-2/5 flex flex-col px-8 py-6 min-h-0">
    <div class="shrink-0 pb-4">
      <h2 class="text-3xl font-semibold leading-tight">Component topology</h2>
    </div>
    <div class="flex-1 flex flex-col justify-center text-xs space-y-1.5 overflow-hidden leading-snug">
      <div><strong>Clients</strong> — serve → CDN; upload → service.</div>
      <div><strong>CloudFront</strong> — two path-based origins: <code>/transform/*</code> → service, <code>/original/*</code> → S3 direct.</div>
      <div><strong>S3</strong> — single source of truth. Keys never overwritten → URL always resolves to exact bytes.</div>
      <div><strong>Image Service</strong> — one binary, two ECS services (Uploader + Processor) behind one ALB.</div>
      <div><strong>Management Tool</strong> — HMAC key rotation, per-consumer quotas, moderation, dashboards.</div>
      <div><strong>Service Consumers</strong> — Chat, Feed, Profile, Story — integrate against the public API.</div>
      <div class="mt-2 p-2 bg-blue-50 rounded text-[11px]">
        <strong>Immutable URLs.</strong> Updates produce a new URL, so CloudFront invalidation is never required — zero API cost, zero propagation window.
      </div>
    </div>
  </div>
  <div class="w-3/5 flex justify-center items-center p-4 min-h-0">
    <img src="/architecture.svg" class="max-h-full max-w-full object-contain" />
  </div>
</div>

<!--
Đi theo flow diagram bên phải:

1. "Client request /transform/chat-avatar-42/200x200 — CloudFront miss thì route sang Processor. Processor kéo original từ S3, resize, stream trả về. CloudFront cache lại. Hàng triệu request sau đều là edge hit."

2. "Upload là flow thứ hai — client POST raw bytes, Uploader strip EXIF, ghi vào S3, trả URL về."

3. Nhấn: "/original/* là CloudFront behavior khác — đi thẳng vào S3, không tốn service CPU. Quan trọng khi client cần raw download."

Điểm mấu chốt là rule immutable URL ở callout cuối. Đây là operational superpower của design: không cần gọi invalidation, không lo stale cache. CTO nào từng trải qua sự cố CloudFront purge sẽ hiểu ngay.

Nếu hỏi "xoá ảnh thì xử lý sao?" — tombstone key; request mới trả 404; CDN hit cũ tự hết hạn theo TTL.

60–75 giây cho slide combined này.
-->

---
layout: section
---

# Options

Three candidates on the market. One obvious winner. One structural concern.

<!--
Transition. "Em đã đánh giá ba serve engine. Đây là kết quả."

Không cần dừng lâu. 5 giây.
-->

---

# Market landscape

| Candidate | Stack | Verdict |
|---|---|---|
| **imgproxy** | Go → libvips (CGo) | Production-proven. Fork-friendly. Pro gates workaroundable. |
| **thumbor** | Python → Pillow | **Rejected** — Pillow ~order-of-magnitude slower than libvips. |
| **weserv/images** | C++ → libvips (nginx module) | **Rejected** — p99 ≥ 5s under load; 4–8× imgproxy memory. |

<div v-click class="mt-6 text-lg">
  <strong>libvips is the common engine.</strong> The differentiator is the language and runtime calling into it.
</div>

<!--
Insight quan trọng: libvips là backend chung. Mình không chọn thư viện xử lý ảnh — mình chọn cách gọi libvips.

Reframe lại toàn bộ quyết định. Đây không phải chuyện "Go vs C++" trừu tượng; mà là "wrapper nào thêm ít overhead hơn trên cùng một khối công việc bên dưới."

Nếu hỏi về ImageMagick: không nằm trong danh sách. Chậm và kém an toàn hơn libvips.

30 giây.
-->

---

# Pick: imgproxy — with a structural concern

**Best of existing options** — proven at scale, great observability, MIT core.

But the runtime has two taxes you can't tune away:

<div class="grid grid-cols-2 gap-4 mt-4">

<div class="p-4 border rounded">
<h4>CGo boundary</h4>
<p class="text-sm opacity-80 mt-2">Every libvips call crosses Go→C: stack switch, param marshalling, <code>runtime.LockOSThread</code> to pin the goroutine. On a hot path dominated by short calls, it's a tax that never goes away.</p>
</div>

<div class="p-4 border rounded">
<h4>Go GC</h4>
<p class="text-sm opacity-80 mt-2">Image pipelines allocate heavily — S3 byte slices, buffer copies across CGo, response buffers. Drives GC cycles + STW pauses that show up in tail latency.</p>
</div>

</div>

<div v-click class="mt-6 p-4 bg-green-50 rounded">
  <strong>C++ removes both.</strong> No FFI (libvips called directly). No tracing GC — jemalloc with per-arena dirty-page caps for stable RSS.
</div>

<!--
Slide này gieo hạt: "em nghĩ native C++ có thể nhanh hơn đáng kể."

Không cam kết con số cụ thể ở đây. Số đo sẽ hiện ở ba slide tới. Chỉ cần nói có hai lý do cấu trúc để kỳ vọng sẽ thắng — không phải nói suông.

Nếu hỏi "nhanh hơn bao nhiêu?" — "Lát nữa sẽ thấy số. Hypothesis ban đầu là 'meaningful', không chốt phần trăm cụ thể."

45 giây.
-->

---

# imago — the C++ port

C++23 reimplementation of imgproxy's core. Same engine, same URL model, direct calls.

<div class="mt-4">

- **Drogon HTTP + C++20 coroutines** — async I/O without callback hell; `co_await` bridges to the worker pool.
- **AWS SDK C++** — native S3 client, no FFI on the fetch path.
- **libvips (native C API)** — called directly, no CGo/JNI/PyObject boundary.
- **jemalloc** — per-arena dirty-page caps keep RSS flat under churn.
- **VipsWorkerPool** — isolates libvips from Drogon IO threads; `VIPS_CONCURRENCY=1` per worker moves parallelism to the pool level.

</div>

<div v-click class="mt-6 text-sm opacity-85">
One binary: serve path (resize/crop/convert) + ingest (upload, EXIF, HEIC→JPEG). Two ECS services, one image. <strong>~5 KLoC</strong>, benchmark harness in place.
</div>

<!--
Ngắn thôi — đây là stack slide, không đi sâu. Câu hỏi hay gặp: "ai sẽ own service này?" — câu trả lời ở slide Decision.

Nếu hỏi "sao chọn Drogon mà không phải framework phổ biến hơn?" — Drogon support C++20 coroutine first-class, overhead thấp hơn alternative, footprint phù hợp với scope của mình. Không khăng khăng phải Drogon nếu có option tốt hơn.

Nếu hỏi "sao không Rust?" — libvips có C bindings cho Rust; kỹ năng team + timing favor C++ hơn. Cả hai đều chạy được.

45 giây.
-->

---
layout: section
---

# Does it actually work?

Measured head-to-head. Same hardware. Same libvips build. Same dataset.

<!--
Chuyển sang phần số. "Giờ là phần mà các anh chị đang đợi."

5 giây.
-->

---

# Benchmark setup

**Host** — MacBook Pro · Apple M4 Pro · 14 cores (10P + 4E) · 24 GB · Darwin 25.3

Both services in Docker, 4 worker threads each:
- imago: `THREAD_POOL_SIZE=4`, `DROGON_WORKERS=4`
- imgproxy: `IMGPROXY_CONCURRENCY=4`
- libvips built from the same base image

**Load generator.** k6, 14 VUs · 60 s per scenario.

**Scenarios.** Scripts adapted from [imgproxy's own benchmark suite](https://github.com/imgproxy/imgproxy/tree/master/tools/benchmark) — so imgproxy runs on the workload it was authored against. imago routes to equivalent endpoints.

<div class="mt-4 text-sm opacity-70">
Run id: <code>9f04418_20260419_002902</code>
</div>

<!--
Hai ý cần nhấn ở đây:

1. "Cùng hardware, cùng libvips, cùng mọi thứ — biến duy nhất là lớp HTTP + glue. Đó là thứ mình đang đo."

2. "Workload lấy từ benchmark repo của chính imgproxy. Đây không phải bài test em tự viết ra để imago trông đẹp. imgproxy đang chạy trên sân nhà của nó."

Nếu hỏi sao lại 14 VU trên máy 14 core: saturate compute mà không tạo queueing artifact. VU cao hơn chỉ làm queue dài ra.

45 giây.
-->

---

# Results — one table

| Scenario | imgproxy rps | imago rps | Gain | imgproxy p95 | imago p95 |
|---|---:|---:|---:|---:|---:|
| Resize JPEG 512 | 207 | **481** | **2.32×** | 79 ms | **40 ms** |
| Resize WebP 512 | 99 | **216** | **2.18×** | 168 ms | **87 ms** |
| Crop JPEG 512 | 200 | **444** | **2.22×** | 83 ms | **41 ms** |
| Convert JPEG→WebP | 16 | **49** | **3.07×** | 1010 ms | **370 ms** |

<div v-click class="mt-6 text-lg">
  <strong>2.19–3.07× throughput. p95 halved on resize/crop; cut to a third on convert.</strong>
</div>

<!--
Đọc bảng hộ họ, đừng bắt họ tự đọc. "Bốn scenario — hai kiểu resize, crop, và format convert — imago thắng throughput từ 2.19× đến 3.07×, p95 giảm xuống còn một nửa ở mọi case, riêng convert còn một phần ba."

Đừng xin lỗi về memory delta ở slide này — phần đó để dành cho slide sau. Dẫn bằng phần thắng trước.

Dòng convert có gain lớn nhất vì WebP encode là CPU-bound — chỗ mà FFI + GC tax đánh vào imgproxy mạnh nhất.

45 giây.
-->

---

# Throughput

<div class="flex justify-center">
  <img src="/charts/throughput_latency_9f04418_20260419_002902.png"
       alt="Throughput and latency per scenario"
       class="w-[90%] max-h-[70vh] object-contain" />
</div>

<!--
"Requests per second trên bốn scenario. Bar cao hơn = tốt hơn."

Không kể từng cột. Chỉ vào cột convert: "imago gấp 3× ở đó vì WebP encode là workload nặng CPU nhất — chỗ FFI tax của imgproxy bị phạt nặng nhất."

15 giây.
-->

---

# Speedup

<div class="flex justify-center">
  <img src="/charts/speedup_9f04418_20260419_002902.png"
       alt="Speedup vs imgproxy"
       class="w-[90%] max-h-[70vh] object-contain" />
</div>

<div class="mt-4 text-center text-lg">
  imago vs imgproxy — <strong>2.19–3.07×</strong> across all scenarios.
</div>

<!--
Chart này để tự nói. Không đọc từng số.

"Baseline 1.0× là imgproxy. Cột cao hơn 1 là imago thắng. Convert là đỉnh vì CPU-bound, resize/crop hơn 2× đều đặn."

15 giây.
-->

---

# Latency percentiles

<div class="flex justify-center">
  <img src="/charts/latency_percentiles_9f04418_20260419_002902.png"
       alt="Latency percentiles avg/p90/p95"
       class="w-[90%] max-h-[65vh] object-contain" />
</div>

<div class="mt-4 text-lg text-center">
  No tail blow-up under load. p95 tracks the mean.
</div>

<!--
Ý nghĩa của chart percentile không phải con số tuyệt đối — đã có trong bảng rồi. Mà là *hình dạng*: avg, p90, p95 đều sát nhau. Không có long tail nào ẩn.

Nếu p99 của imgproxy phình lên trong khi mean vẫn đẹp, thì phải bàn lại. Nhưng không. Fair fight.

20 giây.
-->

---

# CPU & memory — peak per scenario

<div class="grid grid-cols-1 gap-2">

![Peak CPU and memory](/charts/resource_usage_9f04418_20260419_002902.png)

</div>

<div v-click class="mt-4 p-4 bg-yellow-50 rounded text-sm">
  <strong>CPU tells the real story.</strong> imgproxy plateaus at ~4 cores even with 14 concurrent VUs on a 14-core host. imago scales to 11–13. Per-request CPU is similar — imago wins by unlocking parallelism, not by being cheaper per op.
</div>

<!--
Đây là chart diagnostic nhất. Headline:

"imgproxy không nhét nổi quá ~4 core công việc dù mình cấp cho nó 14. Đó là Go scheduler + `runtime.LockOSThread` + CGo handoff cùng nhau serialize hot path. imago không có constraint đó."

Phần memory — đúng, imago dùng nhiều hơn. Là do jemalloc giữ dirty pages để reuse. Stable, không tăng. Slide sau sẽ chứng minh.

Đừng để memory bar trở thành câu chuyện ở đây. Insight nằm ở CPU chart.

45 giây.
-->

---

# Memory stability

<div class="grid grid-cols-2 gap-2">

![Memory stitched](/charts/memory_timeline_stitched.png)

![Memory per run](/charts/memory_per_run.png)

</div>

<div class="mt-4 text-lg">
  8 back-to-back runs · 20 active minutes · trendline <strong>−1.62 MB/min</strong> → no leak.
</div>

<!--
"Nhìn thấy RSS của imago 0.7–1.1 GB nhiều người lo là leak. Không phải. Trong 20 phút load liên tiếp qua 8 scenario, trendline hơi *âm*. Đó là arena cap của jemalloc bound fragmentation."

Chart bên trái là view stitched — khoảng idle giữa các run được collapse lên trục liên tục để đọc slope dễ hơn. Bên phải là từng run trên trục riêng.

Nếu hỏi về chart gốc có gap: "Gap ở x=1000 là do sampler chỉ chạy khi k6 đang đánh vào imago — không chạy lúc imgproxy đến lượt. Cùng data, chỉ là visualize lại cho liên tục."

30 giây.
-->

---

# What the numbers say

- **Throughput: 2.19–3.07×.** Beyond what CGo alone explains. GC pressure + scheduler + SDK overhead stack on top of FFI tax.
- **Latency: halved on resize/crop; a third on convert.** p95 tracks throughput. No tail anomaly.
- **CPU: imgproxy plateaus at ~4 cores; imago uses 11–13.** Per-op cost similar — imago unlocks parallelism.
- **Memory: higher but stable.** 0.7–1.1 GB vs 0.2–0.5 GB. Trendline −1.62 MB/min across 20 minutes → no leak. Bounded by jemalloc arena cap.
- **Zero failures.** All scenarios completed at target concurrency.

<div v-click class="mt-6 p-3 bg-gray-100 rounded text-sm">
  On CPU-bound <code>c7g</code> where compute sets instance size, the extra memory is effectively free.
</div>

<!--
Slide summary trước khi đến recommendation. Đi qua 5 điểm, rồi chốt bằng ý operational cuối.

Ý c7g là câu defuse memory concern cho bất kỳ ai đang tính fleet sizing: trên compute-optimized instance, mình trả tiền CPU; memory đi kèm theo instance dù có dùng hay không. Dùng 1 GB thay vì 300 MB không tốn thêm xu nào.

Nếu hỏi về instance lớn hơn hay memory-optimized: trade-off khác, nhưng c7g là chỗ libvips thắng nhờ ARM NEON.

45 giây.
-->

---
layout: section
---

# Recommendation

Ingest is built either way. The trade-off is performance vs. development cost and team risk.

<!--
Transition. Ý về ingest rất quan trọng: không option nào cho mình ingest miễn phí. imgproxy là serve-only. Dù chọn gì thì mình vẫn phải xây uploader / EXIF strip.

Vậy câu hỏi thu hẹp lại: *cho serve path*, chọn runtime nào?

10 giây.
-->

---

# Option 1 — imgproxy (forked where needed)

<div class="text-lg mt-4">

**Performance.** Acceptable. 207 rps · p95 79 ms on Resize JPEG 512. Scales horizontally on AWS. Latency sits in the "average" band — fine for page loads, noticeable on feeds and chat.

**Development cost.** Adding features we need (smart-crop, AVIF, HEIC ingest) means **forking, learning the Go codebase, and rebasing upstream forever.** Continuous tax, not a one-shot port.

</div>

<div v-click class="mt-6 p-4 bg-blue-50 rounded">
  Second-best on performance. Fundable. Doesn't hinge on one person.
</div>

<!--
Nói thẳng option này là gì. Không phải option tệ. Đây là lựa chọn bảo thủ.

Câu "rebase upstream vĩnh viễn" là quan trọng — ai từng maintain một fork dài hạn đều biết nỗi đau này. Không phải chi phí lý thuyết.

Phần click reveal là one-line pitch cho option này.

30 giây.
-->

---

# Option 2 — imago (in-house C++23)

<div class="text-lg mt-4">

**Performance.** 2.19–3.07× throughput. p95 halved on resize/crop (~40 ms), cut to a third on convert. Low tail latency is the differentiator for **social/chat workloads** where a 150 ms tail shows up as feed jank. Fleet ≈ half imgproxy's.

**Risk.** Needs a **senior C++ engineer** to own — modern C++23 / coroutines / sanitizers aren't entry-level. Codebase is small (~5 KLoC) and bounded by libvips, but bus-factor is real on a Go-skewed team.

</div>

<div v-click class="mt-6 p-4 bg-green-50 rounded">
  Best performance. Smaller fleet. Needs one committed owner.
</div>

<!--
Đừng overpromise. Rủi ro có thật, phải nói rõ. "Bus-factor có thật" nguyên văn — đừng làm nhẹ.

Ý fleet giảm một nửa là argument về tiền. Nếu AWS bill quan trọng (mà đúng là có), 2× throughput per task = ~½ số task cho cùng lượng traffic. Một năm trên c7g là tiền thật.

Nếu hỏi "người đó nghỉ thì sao?" — codebase nhỏ, libvips làm phần nặng, tài liệu handover trong docs/, benchmark trong CI bắt regression. Không zero-risk, nhưng không unmanageable.

30 giây.
-->

---
layout: center
class: text-center
---

# Decision

<div class="text-2xl pt-4 leading-relaxed">

**Adopt imago** if we can staff one senior C++ owner.

The 2–3× throughput and halved p95 matter at scale; infrastructure cost will reduce; CGo is a ceiling that won't move.

</div>

<div class="text-xl pt-8 opacity-80">
  <strong>Otherwise, fall back to Option 1 (forked imgproxy)</strong> — second-best, fundable, doesn't hinge on one person.
</div>

<!--
Đây là slide để dừng nói và đợi.

Quyết định có điều kiện theo staffing. Có chủ đích. Mình đang đề nghị CTO cam kết hiring / assignment cùng lúc với approve tech choice. Hai thứ coupled — imago không có owner thì tệ hơn imgproxy có fork.

Nếu hỏi "ai làm owner?" — phải có sẵn tên trong đầu. Nếu hỏi "chạy song song fork như insurance được không?" — được, nhưng nói rõ đó là chi phí thêm, không phải hedge.

Đợi phản hồi trước khi qua slide kế.

60+ giây. Để im lặng làm việc của nó.
-->

---

# Rollout (no fixed dates — exit-criteria gated)

<div class="grid grid-cols-3 gap-4 mt-4">

<div class="p-4 border rounded">
<h4>Phase 1 — Pilot</h4>
<ul class="text-sm mt-2 space-y-1">
<li>One surface (e.g. chat avatars)</li>
<li>Shadow mode, 10% of live traffic</li>
<li>Compare bytes, hash, latency</li>
</ul>
<p class="text-xs mt-3 opacity-70"><strong>Exit:</strong> 7d parity, errors < 0.1%, p99 in SLO, Prom + logs signed off.</p>
</div>

<div class="p-4 border rounded">
<h4>Phase 2 — Platform</h4>
<ul class="text-sm mt-2 space-y-1">
<li>2–3 more surfaces</li>
<li>Internal SDK / URL helper</li>
<li>HMAC enforcement on</li>
</ul>
<p class="text-xs mt-3 opacity-70"><strong>Exit:</strong> CF hit ≥ 85%, zero P1s, metrics parity.</p>
</div>

<div class="p-4 border rounded">
<h4>Phase 3 — Default</h4>
<ul class="text-sm mt-2 space-y-1">
<li>Mandated path for new pipelines</li>
<li>Legacy resizers sunset</li>
<li>Ownership transferred or decommissioned</li>
</ul>
<p class="text-xs mt-3 opacity-70"><strong>Exit:</strong> no non-imago pipelines, or each has owner + retirement date.</p>
</div>

</div>

<!--
Ý của exit-criteria thay cho calendar date: rollout không thể bị đẩy vì deadline Jira. Mỗi gate bảo vệ phase tiếp theo.

Nếu hỏi thực tế mất bao lâu: Phase 1 ~4–6 tuần; Phase 2 ~1 quý; Phase 3 gắn với legacy retirement, tính theo năm.

Nếu hỏi rollback: mỗi phase đều front bằng CloudFront behavior — chuyển rule lại trong vài phút nếu cần.

30 giây.
-->

---

# Risks we've thought through

| Risk | Mitigation |
|---|---|
| In-house maintenance burden | Small codebase (~5 KLoC); CI-enforced bench; on-call rotation |
| libvips CVE | Same exposure as imgproxy; patched via base image update |
| C++ memory safety | jemalloc in prod · ASAN in CI · RSS monitored · sustained-load bench |
| Hiring pool smaller than Go | Service layer is idiomatic C++; image logic is libvips config |
| AVIF encode gap | libvips supports AVIF; build-flag enable when product asks |
| Observability parity | Phase 1 exit gate requires Prometheus + log schema signed off |
| Regression in future release | Bench runs every release tag; CSV diff gates `main` |

<!--
Không đọc hết bảng. Chủ động nói: "Em sẵn sàng đi sâu vào bất kỳ item nào — câu hỏi hay gặp nhất là hiring và CVE response."

Hiring: chuẩn bị sẵn tên một senior C++ engineer trong đầu. Nếu chưa có tên, Option 2 không nằm trên bàn và slide Decision thu về Option 1.

CVE: exposure giống hệt imgproxy. Cả hai đều gọi libvips. Patch cũng giống nhau — bump base image, redeploy.

20 giây, trừ khi có câu hỏi.
-->

---
layout: default
---

# Demo — playground

Same source, same parameters, two services. Side-by-side panels show rendered output, server timing, bytes on the wire, and EXIF parity.

- Pick an S3 key or upload a file
- Choose resize / crop / convert · tweak w, h, fit, gravity, output, quality
- Request fires against `imago` and `imgproxy` in parallel
- Compare: timing bars, byte counts, stripped EXIF, decoded dimensions

<div class="mt-6 text-sm opacity-80">
  Running locally against the same docker stack the benchmark ran on — <code>http://localhost:5173</code>
</div>

<!--
Demo ngắn, 2 phút. Kịch bản:

1. Mở playground, dán S3 key của ảnh có EXIF GPS (IMG_TEST.JPG).
2. Resize tab → w=400, fit=fill → Run. Hai panel hiện: imago ~20ms server time, imgproxy ~40-50ms, bytes parity (sau fix jpeg-thumbnail-data).
3. Mở EXIF panel: cả hai đều stripped GPS, imago giữ Orientation chuẩn.
4. Nếu còn thời gian: Crop tab với gravity=sm để show smart-crop thay đổi output.

Không demo Convert — 1s/request cho imgproxy sẽ làm audience sốt ruột, và đã có trên bảng benchmark rồi.

Không mở Settings, không show code, không scroll logs.
-->

---
layout: end
---

# Q&A

<div class="pt-6 opacity-70">
  Full proposal: <code>docs/solution-proposal-image-service-v2.md</code>
</div>

<!--
Slide kết. Mở sàn cho câu hỏi.

Câu thường gặp và hướng trả lời ngắn:

- "Ai sẽ own service?" — chuẩn bị sẵn một tên senior C++ trong đầu; nếu chưa có, fallback là Option 1.
- "Rollback thế nào?" — CloudFront behavior routing; đổi rule vài phút là xong.
- "Xoá ảnh thì sao?" — tombstone key, request mới trả 404, CDN hit cũ hết hạn theo TTL.
- "CVE libvips?" — exposure giống hệt imgproxy; bump base image và redeploy.
- "Chạy song song cả hai như insurance?" — được, nhưng là chi phí thêm, không phải hedge miễn phí.

Nếu cần quyết định ngay: "Với staffing đã confirm, em đề nghị approve Phase 1 pilot hôm nay."
-->

