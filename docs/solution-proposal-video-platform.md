# Solution Proposal: Company-Wide Video Streaming Platform

**Audience:** CTO, Engineering Director
**Author:** Engineering
**Date:** 2026-04-20
**Status:** For approval

---

## 1. Context

We currently have no shared video infrastructure. Problems this platform is designed to address:

- **No video ingest pipeline.** There is no production path for accepting uploaded video files (MP4, AVI, MOV) or live streams via RTMP or WebRTC. Each product either avoids video entirely or relies on third-party embeds with no internal ownership.
- **No transcoding capability.** We have no ability to transcode source video into adaptive bitrate formats (HLS, DASH). Serving raw upload files directly is not viable at scale — incompatible codecs, excessive bandwidth, and no ABR for varying network conditions.
- **No video delivery layer.** There is no CDN-integrated delivery path for video segments or manifests. Without this, even correctly transcoded content cannot be served reliably to large audiences.
- **No live streaming support.** RTMP ingest (for OBS/hardware encoders) and WebRTC ingest (for browser-based broadcast) are completely absent. Any live streaming feature requires building everything from scratch.
- **No ancillary services.** Subtitle embedding, thumbnail extraction, and secure playback URL generation do not exist as shared capabilities. Each product would need to build them independently.
- **No CMS or operational tooling.** There is no operator interface for managing video assets, monitoring transcoding jobs, or debugging delivery issues.

**Goal:** build one video platform — ingest, transcode, store, deliver — that all products can adopt, with a clean internal API, a CMS for operators, and a client-side SDK for consumers.

---

## 2. Essential Features

### 2.1 Must-Have

| Feature | Capability | Priority |
|---|---|---|
| **Upload (VOD)** | Resumable multipart upload for MP4, AVI, MOV. Magic-byte validation, codec check, size and duration caps enforced before processing. | Critical |
| **RTMP Ingest** | Receive live streams from OBS and hardware encoders. Stream-key authentication. Webhook on connect/disconnect. | Critical |
| **WebRTC Ingest** | Browser-based live broadcast via WebRTC (WHIP protocol). Re-publishes internally to the same live pipeline as RTMP. | Critical |
| **Transcoding** | Multi-rendition ladder (1080p / 720p / 480p / 360p / audio-only). Per-job queue with retry and dead-letter queue. | Critical |
| **HLS Delivery** | VOD and live HLS (`.m3u8` + `.ts`). Low-Latency HLS (LL-HLS) for sub-2 s live. ABR master manifest. | Critical |
| **DASH Delivery** | VOD and live MPEG-DASH (`.mpd` + `.m4s` fMP4). LL-DASH for live. | Critical |
| **MP4 Download** | Signed, time-limited download URL for source or a specific rendition. | High |
| **Subtitle** | SRT/VTT ingest, storage, injection into HLS/DASH manifests as sidecar text tracks. | High |
| **Thumbnail Extractor** | Auto-extract frames at ingest (interval + scene-change). On-demand frame extraction API. | High |
| **Playback URL** | Signed playback token (JWT/HMAC). CDN enforces expiry and optional IP binding. | High |
| **Delete Video** | Soft-delete with configurable retention. Hard delete purges storage and issues CDN invalidation. | High |

### 2.2 Nice-to-Have

| Feature | Capability | Priority |
|---|---|---|
| **DRM** | Widevine + FairPlay envelope. Key management via KMS. Required for premium content. | High |
| **Preview Sprite** | Storyboard sprite sheet (WebVTT + JPEG strip) for player hover-scrub preview. | Medium |
| **Clip / Trim** | Server-side trim API: re-package existing segments without re-encoding. | Medium |
| **Chapters** | Chapter marker metadata embedded in manifests for player scrubbing. | Medium |
| **Multi-audio tracks** | Multiple language audio tracks muxed as separate EAC-3/AAC streams in HLS/DASH. | Medium |
| **Live Recording** | Auto-archive live stream to VOD pipeline on stream end. | Low |
| **Per-title Encode** | VMAF-guided bitrate selection per video to minimise bitrate at target quality. | Low |

---

## 3. Architecture Overview

The platform is organised as six horizontal layers — **Actors → Ingest → Transcode → Storage → Delivery → Consumers** — built incrementally across four phases. Each phase is independently deployable and leaves the previous phase fully operational.

### 3.1 Phase-by-Phase Architecture

```
┌──────────────┬─────────────────────┬──────────────────────┬─────────────────────┬─────────────────────┬─────────────────────┐
│   ACTORS     │       INGEST        │      TRANSCODE       │      STORAGE        │      DELIVERY       │     CONSUMERS       │
├──────────────┼─────────────────────┼──────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┤
│                                                                                                                              │
│  PHASE 1 — Core foundation: VOD upload · transcode to multiple resolutions · HLS/DASH · SDK                                 │
│                                                                                                                              │
│  Web client  │  Upload service     │  C++ FFmpeg workers  │  S3                 │  CDN                │  Web SDK            │
│  Upload      │  Multipart resumable│  1080p·720p·480p·360p│  /raw    (7d)       │  CloudFront/Fastly  │  hls.js + dash.js   │
│  MP4/AVI/MOV │  Validate·magic     │  + audio-only        │  /vod/hls           │                     │                     │
│              │  bytes·codec check  │                      │  /vod/dash          │  Playback URL svc   │  iOS SDK            │
│  Mobile      │                     │  Temporal/K8s jobs   │  (permanent)        │  JWT · signed token │  AVPlayer wrapper   │
│  iOS/Android │  Go API gateway     │  Orchestrator·retry  │                     │                     │                     │
│              │  Auth · rate limit  │  · DLQ               │  PostgreSQL         │  MP4 download       │  Android SDK        │
│  CMS admin   │                     │                      │  Video meta         │  Signed S3 URL      │  ExoPlayer wrapper  │
│  Manage      │  Kafka / SQS        │  Shaka packager      │  Job state          │  1h TTL             │                     │
│  Monitor     │  Job event queue    │  HLS + DASH output   │                     │                     │  Internal services  │
│              │                     │                      │  Redis              │                     │  Feed·chat·profile  │
│  Consumer svc│                     │                      │  Cache·manifest     │                     │                     │
│  Internal API│                     │                      │                     │                     │                     │
│              │                     │                      │                     │                     │                     │
├──────────────┼─────────────────────┼──────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┤
│                                                                                                                              │
│  PHASE 2 — RTMP live streaming                                                                                               │
│                                                                                                                              │
│  OBS/encoder │  SRS 5.x RTMP       │  C++ live workers    │  S3 /live/{key}/    │  CDN live delivery  │  SDK live mode      │
│  Hardware/SW │  Auth via HTTP hook  │  Segment every 2s    │  Rolling 12h window │  max-age=2 manifest │  LL-HLS flag        │
│              │  Forward to workers │  LL-HLS partial 200ms│  Auto-expire        │  max-age=86400 segs │  Manifest polling   │
│  Stream admin│                     │                      │                     │                     │                     │
│  Key mgmt    │  Stream key service │  Kafka consumer group│  Redis live manifest│  CMS live dashboard │  Viewer clients     │
│  Monitor     │  Create·revoke keys │  stream.started evts │  SETEX TTL=10s      │  Bitrate·lag·viewers│  Web·iOS·Android    │
│              │                     │                      │                     │                     │                     │
├──────────────┼─────────────────────┼──────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┤
│                                                                                                                              │
│  PHASE 3 — WebRTC live streaming                                                                                             │
│                                                                                                                              │
│  Browser     │  SRS 5.x WHIP       │  Same live workers   │  Same S3 live path  │  LL-HLS / LL-DASH   │  SDK WebRTC mode    │
│  broadcaster │  WebRTC → RTMP      │  WebRTC arrives as   │  /live/{stream_key}/│  Partial segs       │  lowLatency: true   │
│  WHIP client │  rtc_to_rtmp = on   │  RTMP internally     │  Identical to Ph. 2 │  <2s glass-to-glass │  Subtitle renderer  │
│              │                     │  No code change      │                     │                     │                     │
│  WebRTC      │  STUN/TURN server   │                      │  Subtitle service   │  Thumbnail CDN      │  Browser viewer     │
│  viewer      │  NAT traversal      │  Live thumbnail snap │  SRT/VTT→manifests  │  Live preview URL   │  Native WebRTC path │
│              │                     │  every 60s           │                     │                     │                     │
│              │                     │                      │                     │                     │                     │
├──────────────┼─────────────────────┼──────────────────────┼─────────────────────┼─────────────────────┼─────────────────────┤
│                                                                                                                              │
│  PHASE 4 — Platform: DRM · multi-audio · live recording · per-title encode · sprite · chapters                               │
│                                                                                                                              │
│  Premium     │  Live recording svc │  Per-title encode    │  DRM key store      │  DRM delivery       │  SDK DRM support    │
│  viewer      │  Stream end → VOD   │  VMAF-guided bitrate │  AWS KMS            │  Widevine·FairPlay  │  EME·license fetch  │
│  DRM-gated   │                     │                      │  Never in S3        │                     │                     │
│              │  DRM key service    │  Multi-audio mux     │                     │  Hover scrub CDN    │  SDK chapters UI    │
│  Content     │  AWS KMS envelope   │  EAC-3/AAC tracks    │  Sprite + chapters  │  Sprite VTT delivery│  Scrub·chapter marks│
│  owner       │                     │                      │  /thumbnails        │                     │                     │
│              │  Clip / trim API    │  Sprite storyboard   │  /chapters          │  Chapter manifest   │  SDK audio switcher │
│  CMS operator│  Re-package         │  WebVTT + JPEG strip │                     │  EXT-X-DATERANGE    │  Multi-lang select  │
│  Chapter/clip│  no re-encode       │                      │  Multi-audio tracks │                     │                     │
│              │                     │                      │  /vod/{id}/audio/   │                     │  Analytics SDK      │
│  Analytics   │                     │                      │                     │                     │  QoE·stall·quality  │
│  service     │                     │                      │                     │                     │                     │
└──────────────┴─────────────────────┴──────────────────────┴─────────────────────┴─────────────────────┴─────────────────────┘

Shared across all phases: Kafka · PostgreSQL · Redis · Go API gateway · React CMS
```

### 3.2 Components

| Component | Responsibility |
|---|---|
| **Upload Service** | Resumable multipart HTTP. Validates codec and mime type. Writes raw file to S3. Emits Kafka event on complete. |
| **RTMP Server (SRS 5.x)** | Receives live streams from OBS/encoders. Authenticates stream key via HTTP hook. Forwards to live workers. |
| **WebRTC Gateway (SRS WHIP)** | Accepts WHIP from browsers. Re-publishes internally as RTMP into the same live pipeline. No separate component needed — SRS 5.x handles both. |
| **Job Queue** | Kafka or SQS. Decouples ingest events from transcoding workers. Per-job dead-letter queue with retry. |
| **C++ FFmpeg Workers** | Stateless pods. VOD: full file transcode via `libav*` directly. Live: segment-on-the-fly every 2 s. Write segments directly to S3. |
| **Shaka Packager** | Packages fMP4 into DASH manifests. HLS packaging via FFmpeg. |
| **Object Storage** | S3 / GCS. Raw uploads, HLS+DASH segments, subtitles, thumbnails. Lifecycle rules per prefix. |
| **Metadata DB** | PostgreSQL for video records, job state, permissions. Redis for live manifest state and caching. |
| **CDN** | CloudFront / Fastly. Caches all static segments. Short TTL (2 s) for live manifests. |
| **API Gateway** | JWT auth, rate limiting, signed playback URL issuance. |
| **CMS** | Internal React web console. Video management, transcode monitoring, subtitle upload, thumbnail selection, live dashboard. |
| **SDK** | hls.js/dash.js wrappers + subtitle renderer. Native iOS/Android player wrappers. |

### 3.3 Data Flows

**VOD Upload → Playback**
1. Client uploads file via multipart HTTP to Upload Service.
2. Upload Service validates, writes raw file to S3 `/raw/{video_id}`, emits `video.uploaded` Kafka event.
3. Orchestrator picks up event, spawns FFmpeg workers per rendition profile in parallel.
4. Each worker transcodes → writes `.ts` / `.m4s` segments + manifests to S3 `/vod/{video_id}/`.
5. Orchestrator marks job `READY` in PostgreSQL; CMS reflects status.
6. Client requests playback URL → API Gateway issues signed CDN URL → player loads HLS/DASH manifest.

**RTMP Live → Viewers**
1. Broadcaster connects OBS → RTMP Server. Server fires `on_publish` HTTP hook.
2. API Service validates stream key, emits `stream.started` Kafka event.
3. Live FFmpeg Worker connects to stream, segments every 2 s to HLS/DASH, uploads to S3 `/live/{key}/`.
4. Redis holds a rolling manifest pointer with 10 s TTL.
5. Viewer requests manifest → CDN (2 s TTL) → S3 → player polls for new segments.

**WebRTC Live → Viewers**
1. Browser publishes via WHIP to SRS 5.x WHIP endpoint.
2. SRS re-publishes the media internally as RTMP (`rtc_to_rtmp = on`).
3. Remainder is identical to the RTMP live flow above — no code change in the live worker.

---

## 4. Options

### 4.1 Option Landscape

| Layer | Option 1: Open Source | Option 2: Full AWS | Option 3: In-House |
|---|---|---|---|
| RTMP Server | SRS or nginx-rtmp | AWS IVS | Custom SRS build |
| WebRTC Gateway | mediasoup or Janus | Amazon Kinesis Video | SRS 5.x (WHIP built-in) |
| Transcoding | FFmpeg + K8s Jobs | AWS Elemental MediaConvert | C++ FFmpeg wrapper on K8s |
| Live Packaging | FFmpeg HLS + Shaka Packager | AWS MediaPackage | Custom C++ packager |
| Object Storage | MinIO (self-hosted) or S3 | Amazon S3 | MinIO on-prem or S3 |
| CDN | Nginx + Varnish or CloudFront | Amazon CloudFront | Nginx with custom purge API |
| API Layer | Go / Node.js | AWS Lambda + API Gateway | Go or Java REST/gRPC |
| Metadata DB | PostgreSQL + Redis | Amazon RDS Aurora + ElastiCache | PostgreSQL + Redis on K8s |
| Job Queue | Kafka (self-hosted or Confluent) | Amazon SQS + EventBridge | Kafka on K8s (Strimzi) |
| CMS / Monitoring | Retool or custom React | AWS Console + CloudWatch | Custom React CMS + Grafana |
| Player SDK | hls.js + dash.js | AWS Amplify + hls.js | In-house wrapper over hls.js / ExoPlayer |

---

### 4.2 Option 1 — Compose Open Source

**Stack:** SRS + mediasoup + FFmpeg + Shaka Packager + MinIO/S3 + Kafka + Go API + hls.js/dash.js SDK

Assemble best-of-breed OSS components, each owned by its respective community, glued together with a thin Go orchestration layer and Kafka events.

**Strengths**
- No license cost. Every component is Apache 2.0 or MIT.
- Highly composable — swap any single component independently.
- Strong community support; known operational runbooks for each piece.
- No vendor lock-in; data portability is straightforward.

**Weaknesses**
- Integration burden. Each component has its own configuration model, monitoring surface, and failure mode.
- Running SRS, mediasoup, Kafka, MinIO, PostgreSQL, Redis, and a K8s cluster requires broad infra expertise.
- FFmpeg job management requires custom orchestration (Temporal or hand-rolled state machine).
- LL-HLS/LL-DASH tuning across multiple components is non-trivial.

**Best fit:** teams that want full ownership and have strong DevOps capacity.

---

### 4.3 Option 2 — Full AWS Managed

**Stack:** AWS IVS + S3 + MediaConvert + MediaPackage + CloudFront + API Gateway + Lambda + RDS Aurora + ElastiCache

Use AWS-managed video services end-to-end. Engineering focuses on API integration and business logic, not infrastructure.

**Strengths**
- Fastest path to production. IVS, MediaConvert, and MediaPackage are fully managed — no cluster to operate.
- Built-in HA, scaling, and SLA. No on-call for RTMP server or transcoder fleet.
- Native integration between services: S3 → MediaConvert → MediaPackage → CloudFront with minimal glue.
- DRM (Widevine + FairPlay) available via MediaPackage + SPEKE without a custom KMS.

**Weaknesses**
- Cost at scale. MediaConvert pricing scales with output minutes; MediaPackage CDN-origin charges accumulate for high-egress workloads.
- Hard vendor lock-in. Migrating away from IVS, MediaConvert, or MediaPackage requires rewriting the entire pipeline.
- Less flexibility. MediaConvert's rendition config is less expressive than direct FFmpeg; per-title encode is not supported.
- WebRTC ingest is not natively supported by IVS (RTMP/SRT only); Kinesis Video requires a separate integration model.

**Best fit:** product-focused teams that need to launch quickly and want infrastructure fully operated by AWS.

---

### 4.4 Option 3 — In-House (SRS + C++ Transcoder + Go API)

**Stack:** SRS 5.x + C++ FFmpeg transcoding server + Go API + Kafka + S3 + PostgreSQL + Redis + CloudFront + React CMS

Build a cohesive owned platform where the hot paths (transcode, live segmentation) are written in C++ for maximum throughput with no subprocess overhead, and the API/orchestration layer is idiomatic Go.

**Strengths**
- SRS 5.x natively supports RTMP, SRT, WHIP (WebRTC), and LL-HLS in a single binary — no separate WebRTC gateway component needed in Phase 3.
- C++ FFmpeg server: direct `libav*` calls with no subprocess overhead, no CGo boundary, deterministic memory via jemalloc. 2–3× throughput over subprocess-based approaches at equivalent hardware.
- Full control over rendition logic, segment naming, and manifest structure — enables per-title encode, custom DRM envelope, and future codec adoption (AV1) without upstream dependency.
- Go API: strong concurrency primitives for job orchestration, excellent K8s ecosystem.
- Lowest marginal cost at high volume — no per-minute transcode pricing.

**Weaknesses**
- Requires senior C++ engineers for the transcoding server. C++23 / coroutines / sanitizer discipline is not entry-level.
- Higher initial build time (10–18 weeks to feature parity vs. 2–4 weeks for Option 2).
- Full operational ownership: you run the Kafka cluster, SRS fleet, and C++ worker pool.

**Best fit:** teams with the engineering depth to own a media stack and a volume profile where managed services become cost-prohibitive.

---

### 4.5 Comparison

| Dimension | Option 1: Open Source | Option 2: Full AWS | Option 3: In-House |
|---|---|---|---|
| Time to production | 6–10 weeks | 2–4 weeks | 10–18 weeks |
| Infra cost at scale | Medium | High | Low |
| Vendor lock-in | None | High | None |
| Engineering complexity | High | Low | Very High |
| Performance ceiling | High | Medium | Highest |
| WebRTC support | Via mediasoup | Via Kinesis VS | Native (SRS 5.x) |
| DRM support | Manual integration | Managed (SPEKE) | Manual integration |
| Operational ownership | Full | Minimal | Full |

### 4.6 Recommendation

**Adopt Option 3 (SRS + C++ + Go)** if the team can staff one senior C++ engineer and one Go engineer with media domain knowledge. SRS 5.x removes the need for a separate WebRTC component, and the C++ FFmpeg server eliminates subprocess overhead at the point of highest compute cost.

**Start with Option 2 (Full AWS)** if time-to-market dominates and the team is small. Use AWS IVS for live and MediaConvert for VOD, with a migration path to Option 3 once managed service costs exceed an acceptable threshold.

**Option 1 (Open Source compose)** is a valid middle ground for teams that want ownership without C++ risk, accepting that FFmpeg subprocess management and multi-component integration require sustained infra investment.

---

## 5. Proposed Architecture — Option 3 Detail

### 5.1 Stack

SRS 5.x · C++23 (`libav*` + `libx264`/`libx265`) · Go 1.22 · Kafka · PostgreSQL 16 · Redis 7 · S3 · CloudFront · React CMS

### 5.2 Data Flow

```
═══════════════════════════════════════════════════════════════
 VOD UPLOAD PATH
═══════════════════════════════════════════════════════════════
 Client
   └─ POST /v1/videos/upload  (resumable multipart)
         │
         ▼
 Go Upload Service
   ├─ Validate: magic bytes, codec whitelist, max size (20 GB)
   ├─ Write raw → S3 /raw/{video_id}.{ext}
   └─ Emit Kafka: video.uploaded { video_id, s3_key, meta }
         │
         ▼
 Go Orchestrator (Temporal workflow)
   ├─ Create job record in PostgreSQL (status=QUEUED)
   └─ Dispatch tasks → Kafka: transcode.job { video_id, profile }
         │
         ▼  (N workers in parallel, one per rendition)
 C++ Transcoding Workers
   ├─ Pull from Kafka consumer group
   ├─ Stream raw from S3 via libcurl → libavformat demux
   ├─ libavcodec decode → scale (libswscale) → libx264 encode
   ├─ libavformat mux → HLS (.ts segments + .m3u8)
   ├─ Shaka Packager → DASH (.m4s + .mpd)
   ├─ Upload segments to S3 /vod/{video_id}/{profile}/
   └─ Emit Kafka: transcode.done { video_id, profile }
         │
         ▼
 Go Orchestrator → UPDATE video SET status=READY WHERE id=...

═══════════════════════════════════════════════════════════════
 LIVE RTMP PATH  (Phase 2)
═══════════════════════════════════════════════════════════════
 OBS/Encoder → rtmp://ingest.platform.com/live/{stream_key}
                       │
                       ▼
 SRS 5.x (on_publish HTTP hook → Go API validates stream_key)
   └─ Emits stream.started → Kafka
         │
         ▼
 C++ Live Worker (one process per active stream)
   ├─ Connects to SRS RTMP re-publish endpoint
   ├─ Segments every 2 s → HLS /live/{key}/ (rolling 5-segment window)
   ├─ Uploads segments to S3 every 2 s
   ├─ Updates Redis: SETEX live:manifest:{key} TTL=10s
   └─ LL-HLS: partial segments every 200 ms for sub-2 s latency
         │
         ▼
 CDN (CloudFront) → Viewer
   ├─ .m3u8 manifest: Cache-Control: max-age=2
   └─ .ts segments:   Cache-Control: max-age=86400

═══════════════════════════════════════════════════════════════
 WEBRTC PATH  (Phase 3)
═══════════════════════════════════════════════════════════════
 Browser (WHIP client)
   └─ SRS 5.x WHIP endpoint → internal RTMP re-publish
         └─ Remainder identical to RTMP path above
```

### 5.3 Storage Layout

```
s3://platform-video/
├── raw/                        ← original uploads   lifecycle: expire 7d
│   └── {video_id}.{ext}
├── vod/                        ← transcoded VOD      lifecycle: permanent
│   └── {video_id}/
│       ├── hls/
│       │   ├── master.m3u8
│       │   ├── 1080p/  (seg_000.ts … seg_NNN.ts + 1080p.m3u8)
│       │   ├── 720p/
│       │   ├── 480p/
│       │   └── 360p/
│       └── dash/
│           ├── manifest.mpd
│           └── video/  (init.mp4 + chunk_000.m4s … chunk_NNN.m4s)
├── live/                       ← live segments       lifecycle: expire 12h
│   └── {stream_key}/
│       ├── hls/  (live.m3u8 + seg_NNN.ts)
│       └── dash/ (live.mpd  + chunk_NNN.m4s)
├── subtitles/                  ← subtitle files      lifecycle: permanent
│   └── {video_id}/
│       ├── en.vtt
│       └── vi.vtt
└── thumbnails/                 ← thumbnail images    lifecycle: permanent
    └── {video_id}/
        ├── thumb_00001.jpg … thumb_NNNNN.jpg
        └── sprite.jpg + sprite.vtt  (hover-scrub storyboard)
```

### 5.4 Rendition Ladder

| Profile | Resolution | Video Bitrate | Audio | Segment Duration |
|---|---|---|---|---|
| 1080p | 1920×1080 | 4,500 kbps | 192 kbps AAC | 6 s / 2 s LL |
| 720p | 1280×720 | 2,500 kbps | 128 kbps AAC | 6 s / 2 s LL |
| 480p | 854×480 | 1,000 kbps | 128 kbps AAC | 6 s / 2 s LL |
| 360p | 640×360 | 600 kbps | 96 kbps AAC | 6 s / 2 s LL |
| Audio only | — | — | 96 kbps AAC | 6 s |

### 5.5 C++ Transcoding Server Design

The transcoding server is a multi-threaded C++23 service that wraps `libav*` directly — no FFmpeg subprocess, no shell exec.

- One Kafka consumer thread per worker process. Thread pool of N FFmpeg pipelines (N = vCPU count / 2).
- `libavformat` + `libavcodec` for demux/decode/encode. `libswscale` for scale and pixel format conversion. No CGo, no JNI boundary.
- jemalloc as allocator with per-arena dirty-page caps to keep RSS stable under sustained load.
- S3 streaming via AWS SDK C++: multipart upload of segments as produced — no local disk required.
- For live streams: segment callback fires every `hls_time` seconds; C++ uploads `.ts` and atomically updates Redis manifest key.
- ASAN + UBSan in CI; Valgrind in nightly runs. Sanitizer builds gate merges to `main`.

### 5.6 SRS Configuration (abbreviated)

```nginx
listen              1935;
http_api { enabled on; listen 1985; }

vhost __defaultVhost__ {
  rtc {
    enabled     on;
    rtmp_to_rtc on;   # RTMP ingest → WebRTC playback
    rtc_to_rtmp on;   # WebRTC publish → internal RTMP
  }

  http_hooks {
    enabled        on;
    on_publish     http://api-service:8080/hooks/on-publish;
    on_unpublish   http://api-service:8080/hooks/on-unpublish;
  }

  forward {
    enabled     on;
    destination live-worker-1:1936 live-worker-2:1936;
  }
}
```

### 5.7 Deployment Topology

| Service | Runtime | Scales On | Instance Type |
|---|---|---|---|
| `srs-ingest` | K8s DaemonSet | Stream count (sticky sessions) | c7g.xlarge (4 vCPU) |
| `go-api` | K8s Deployment | CPU / RPS | c7g.medium (2 vCPU) |
| `go-orchestrator` | K8s Deployment | Job queue depth | c7g.medium (2 vCPU) |
| `cpp-vod-worker` | K8s Job per video | Job queue depth | c7g.4xlarge (16 vCPU) |
| `cpp-live-worker` | K8s Deployment | Active stream count | c7g.2xlarge (8 vCPU) |
| `cms-frontend` | K8s Deployment | Web request RPS | t4g.medium (2 vCPU) |
| PostgreSQL | RDS managed | Storage + read replicas | db.r7g.large |
| Redis | ElastiCache Cluster | Memory | cache.r7g.large |
| Kafka | MSK or Strimzi on K8s | Partition throughput | kafka.m5.xlarge × 3 |

---

## 6. CMS & Internal Service APIs

### 6.1 API Surface

| Endpoint | Surface | Description |
|---|---|---|
| `POST /v1/videos/upload` | Upload | Initiate resumable multipart upload. Returns `upload_id` and part URLs. |
| `GET /v1/videos/{id}` | CMS | Video metadata, status, manifest URLs, thumbnail list. |
| `GET /v1/videos/{id}/playback` | SDK | Issues signed CDN playback URL (JWT, 6 h expiry, optional IP binding). |
| `GET /v1/videos/{id}/download` | SDK | Issues signed S3 download URL (1 h expiry). Respects DRM flag. |
| `DELETE /v1/videos/{id}` | CMS | Soft-delete. Schedules hard-delete + CDN purge after 30 days. |
| `POST /v1/videos/{id}/subtitles` | CMS | Upload SRT/VTT file. Async parse and attach to manifests. |
| `GET /v1/videos/{id}/thumbnails` | CMS | List extracted thumbnails. Returns signed CDN URLs. |
| `POST /v1/videos/{id}/thumbnails/select` | CMS | Set the primary display thumbnail. |
| `GET /v1/streams/{key}/status` | CMS | Live stream health: bitrate, fps, segment age, viewer count. |
| `GET /v1/jobs/{id}` | CMS | Transcode job state, progress %, per-rendition status, error log. |
| `GET /v1/jobs` | CMS | Paginated job list with filters (status, date, video_id). |
| `POST /v1/streams/keys` | CMS | Generate a new RTMP stream key scoped to a user or channel. |

### 6.2 CMS Console

The CMS is a React web application backed by the Go API. Core views:

- **Video Library** — searchable, filterable table of all videos; status badges (`QUEUED` / `TRANSCODING` / `READY` / `ERROR`); bulk-delete.
- **Video Detail** — metadata editor, thumbnail selector, subtitle upload, manifest URL copy, embedded playback preview, per-rendition transcode status.
- **Transcode Monitor** — real-time job board: active, queued, and failed jobs. Per-job progress bar, FFmpeg log tail, retry button.
- **Live Stream Dashboard** — active streams with bitrate graph, segment-age indicator (health proxy), viewer count, kill-stream button.
- **Stream Key Management** — create and revoke RTMP keys. Associate keys with a user or channel.

### 6.3 Ancillary Services

**Subtitle Service**
- Accepts SRT or WebVTT upload via CMS or API. Converts SRT → VTT if needed. Stores to S3 `/subtitles/{video_id}/{lang}.vtt`.
- Patches HLS master manifest to include `#EXT-X-MEDIA TYPE=SUBTITLES` entries.
- Patches DASH MPD to add `<AdaptationSet contentType="text">` with VTT segment references.

**Thumbnail Extractor**
- VOD: triggered by `transcode.done` event. FFmpeg extracts one frame every 10 s plus scene-change frames.
- Live: periodic snapshot every 60 s from the live worker, uploaded to S3 and exposed via API for stream preview.
- Generates sprite storyboard (WebVTT + JPEG strip) for hover-scrub in the player SDK.

**Playback URL Service**
- Generates time-limited signed URLs for CDN (CloudFront signed cookies / URL signing) and S3 (presigned URL for downloads).
- JWT payload: `video_id`, `allowed_origins`, `expiry`, optional IP lock, optional geo-restriction.
- CDN enforces token at edge via CloudFront Functions or Lambda@Edge.

---

## 7. SDK

### 7.1 Components

| Component | Implementation | Capabilities |
|---|---|---|
| HLS Playback (Web) | hls.js wrapper | Auto quality selection, stall recovery, live low-latency mode, buffer config |
| DASH Playback (Web) | dash.js wrapper | ABR, Widevine DRM, live/VOD unified API |
| Subtitle Renderer (Web) | Custom VTT parser | Render WebVTT cues, style override, multi-track switcher |
| Playback URL Client | JS / Go client | Fetch signed playback URL from API. Cache token until 90% of TTL. |
| iOS Player | AVPlayer wrapper (Swift) | HLS native playback, FairPlay DRM, subtitle track selection, AirPlay |
| Android Player | ExoPlayer wrapper (Kotlin) | HLS + DASH, Widevine DRM, subtitle renderer, Cast SDK |

### 7.2 Web SDK — Minimal Integration

```javascript
import { VideoPlayer } from '@platform/video-sdk';

const player = new VideoPlayer('#player-container', {
  videoId:    'vid_abc123',
  authToken:  await getAuthToken(),
  mode:       'hls',         // 'hls' | 'dash'
  subtitles:  true,          // auto-load available subtitle tracks
  lowLatency: false,         // true enables LL-HLS for live
});

player.on('qualityChange', ({ from, to }) => analytics.track('quality_change', { from, to }));
player.on('error',         (err)          => logger.error('player_error', err));
player.on('stall',         ({ duration }) => analytics.track('stall', { duration }));

// VOD playback
player.play({ videoId: 'vid_abc123', startAt: 120 });

// Live playback
player.play({ streamKey: 'sk_live_xyz', lowLatency: true });
```

### 7.3 iOS SDK — Minimal Integration

```swift
import PlatformVideoSDK

let player = PlatformPlayer()
player.authToken = await AuthService.getToken()

// VOD
try await player.load(videoId: "vid_abc123")
player.subtitles.select(language: "en")
playerViewController.player = player

// Live
try await player.loadLive(streamKey: "sk_live_xyz", lowLatency: true)
```

### 7.4 Android SDK — Minimal Integration

```kotlin
val player = PlatformPlayer.Builder(context)
    .authToken(authService.getToken())
    .mode(PlaybackMode.HLS)      // or DASH
    .subtitlesEnabled(true)
    .build()

// VOD
player.loadVideo("vid_abc123")
playerView.player = player

// Live
player.loadLiveStream("sk_live_xyz", lowLatency = true)
```

---

## 8. Operating Model

### 8.1 Observability

| Signal | Source | Target |
|---|---|---|
| Metrics | Prometheus `/metrics` on all services | Grafana — ingest RPS, transcode queue depth, segment lag, CDN hit rate |
| Logs | Structured JSON (all services) | CloudWatch Logs / ELK — per-request traces, FFmpeg stderr per job |
| Traces | OpenTelemetry SDK (Go + C++) | Jaeger / AWS X-Ray — end-to-end upload → transcode → manifest latency |
| Alarms | CloudWatch / AlertManager | Transcode queue depth, segment age, CDN hit rate (see §8.2) |
| Health | `GET /health` on every service | K8s liveness + readiness probes every 15 s |

### 8.2 Key Alerts

- `transcode_queue_depth > 50` for 5 min → page on-call.
- `live_segment_age_seconds > 8` (viewer will stall) → page on-call.
- `transcode_error_rate > 5%` over 10 min → page on-call.
- `cdn_hit_rate < 80%` over 15 min → investigate cache headers.
- `srs_publish_failure_rate > 1%` → check ingest network / stream key auth service.

### 8.3 CI/CD

- GitHub Actions: build → unit test → integration test (Docker Compose: SRS + C++ worker + Go API) → push ECR → K8s rolling deploy.
- C++ workers: ASAN + UBSan build on every PR. Valgrind nightly job.
- Go services: race detector enabled in all test runs.
- Canary deploy: 10% traffic to new version for 1 h before full rollout. Auto-rollback on error rate spike.

---

## 9. Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| C++ memory safety (UAF, leaks) | Worker crashes, data corruption | ASAN in CI, Valgrind nightly, jemalloc with RSS monitoring, sanitizer gates on `main` |
| SRS community bus-factor | Unmaintained ingest server | Pin to SRS release tag; maintain an internal fork; RTMP/WHIP spec compliance means drop-in replacement is feasible |
| Kafka outage drops transcode events | Videos stuck in QUEUED | Outbox pattern: write job to PostgreSQL first; Kafka event is best-effort; orchestrator polls DB as fallback |
| Live segment lag spike | Viewer stall and buffering | Segment age alert at 8 s; auto-scale live worker pool; SRS fan-out to multiple workers |
| S3 cost growth from raw uploads | Budget overrun | S3 Lifecycle rule: expire `/raw/` after 7 days; only `/vod/` and `/thumbnails/` are permanent |
| CDN cache miss on live manifests | Origin overload at peak | `Cache-Control: max-age=2` on manifests + CloudFront origin shield reduces origin hits |
| DRM key management complexity | Content leak if keys compromised | Keys stored in AWS KMS; never in S3 or DB plaintext; key rotation API exposed in CMS |
| Hiring: senior C++ scarcity | Slow delivery, bus-factor 1 | Option 1 (subprocess FFmpeg via Go) is a viable fallback; architecture is otherwise identical |

---

## 10. Rollout

No fixed dates. Each phase has explicit exit criteria; the next phase does not start until the previous exits.

### Phase 1 — Core Foundation

Deliver the complete VOD pipeline: upload, transcode to all renditions, HLS/DASH delivery, and the full SDK across all platforms.

- Go Upload Service: resumable multipart upload, magic-byte validation, S3 write, Kafka event.
- C++ VOD transcoding worker: all rendition profiles (1080p → 360p + audio). HLS output via FFmpeg, DASH packaging via Shaka Packager.
- Go Orchestrator: Temporal workflow, job queue, per-rendition parallelism, retry, dead-letter queue.
- S3 storage layout with lifecycle rules. CDN configuration (CloudFront / Fastly).
- Playback URL signing (JWT + CloudFront signed URLs). MP4 download endpoint.
- Thumbnail extractor (interval + scene-change frames). Sprite storyboard.
- Subtitle upload + VTT manifest injection.
- Go API: `/upload`, `/videos/{id}`, `/videos/{id}/playback`, `/videos/{id}/download`, `/videos/{id}/subtitles`, `/jobs/{id}`.
- CMS v1: video library, transcode monitor, subtitle upload, thumbnail selector.
- Web SDK: hls.js wrapper + dash.js wrapper + subtitle renderer.
- iOS SDK (AVPlayer wrapper) and Android SDK (ExoPlayer wrapper).

**Exit:** a product team can upload a video, monitor transcoding to all resolutions in the CMS, and play it back on Web, iOS, and Android using the SDK. CDN hit rate ≥ 85% in staging load test. Zero sanitizer errors in CI.

---

### Phase 2 — RTMP Live Streaming

Add RTMP ingest and live segmentation. All infrastructure from Phase 1 (Kafka, S3, CDN, Redis) is reused unchanged.

- SRS 5.x RTMP server deployed as a K8s DaemonSet with sticky session routing.
- Go API: HTTP `on_publish` / `on_unpublish` hooks for stream key validation.
- Stream key management in CMS: create, revoke, associate to user/channel.
- C++ live worker: RTMP → HLS + DASH segment-on-the-fly every 2 s. Rolling 5-segment window uploaded to S3 `/live/{key}/`. Redis manifest pointer with 10 s TTL.
- LL-HLS: partial segments every 200 ms for sub-2 s latency mode.
- Live thumbnail snapshot every 60 s.
- CMS live dashboard: active streams, bitrate graph, segment-age indicator, viewer count, kill-stream button.
- SDK live mode: low-latency flag, live manifest polling.

**Exit:** a live stream from OBS reaches viewers via LL-HLS with < 3 s glass-to-glass latency in staging. Segment age alert fires correctly. Stream key auth rejects an invalid key.

---

### Phase 3 — WebRTC Live Streaming

Add browser-based broadcasting via WebRTC WHIP. The live worker pipeline from Phase 2 is unchanged — WebRTC simply re-enters the existing RTMP path.

- SRS 5.x WHIP endpoint enabled (`rtc_to_rtmp = on`). WebRTC publish re-enters the existing RTMP live path internally.
- STUN/TURN server for NAT traversal (coturn or managed).
- SDK WebRTC mode: `lowLatency: true`, subtitle renderer enabled for live.
- CMS: WebRTC stream health shown in the same live dashboard as RTMP streams.

**Exit:** a browser (Chrome/Firefox) can publish via WHIP and viewers receive the stream via LL-HLS with < 3 s latency. TURN relay confirmed working across NAT boundaries in staging.

---

### Phase 4 — Platform Features

Harden the platform with DRM, multi-audio, advanced encoding, and live archiving. All existing pipelines remain operational.

- DRM: Widevine + FairPlay via AWS KMS. Key provisioning API in CMS. SDK EME license fetch on Web, FairPlay on iOS, Widevine on Android.
- Multi-audio track mux: separate EAC-3/AAC tracks per language in HLS/DASH manifests. SDK audio switcher UI.
- Per-title encode: VMAF-guided bitrate selection per video, replacing the fixed rendition ladder for new uploads.
- Chapter markers: `EXT-X-DATERANGE` in HLS manifests. CMS chapter editor. SDK chapter scrubber UI.
- Clip / trim API: server-side re-package of existing segments without re-encode.
- Live recording: on `on_unpublish`, trigger a VOD transcode job from the archived live segments.
- Analytics SDK: QoE metrics (stall rate, startup time, rebuffer ratio, quality switches) reported to internal analytics service.

**Exit:** DRM-gated content plays on all three SDK targets. Live recording produces a playable VOD asset within 5 minutes of stream end. No active non-platform video pipelines in production. Zero P1 incidents attributable to the platform in 30 days post-migration.

---

## 11. Decision Requested

Approve the Video Streaming Platform as the company-wide standard and authorise Phase 1 development.

Specifically, approve:

1. **Option 3 (SRS + C++ Transcoding Server + Go API)** as the target architecture, with Option 2 (Full AWS) as the fallback if C++ staffing cannot be confirmed within 30 days.
2. **Phase 1 scope and team allocation** — minimum: 1 senior C++ engineer, 1 Go engineer, 1 frontend engineer (CMS + SDK), 1 DevOps/SRE.
3. **Infrastructure provisioning** — S3 bucket, CDN configuration, and K8s namespace for the staging environment to unblock Phase 1.
4. **Pilot product team onboarding** — two internal product teams to integrate against the API and SDK starting in Phase 2.