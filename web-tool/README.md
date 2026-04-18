# imago web-tool

Browser playground for the imago HTTP API (`/resize`, `/crop`, `/convert`).
Built with Vite + React + TypeScript + Tailwind v4 + shadcn-style primitives.

## Features

- Three tabs, one per imago endpoint: **Resize**, **Crop**, **Convert**.
- Per-tab controls for every server-side query parameter:
  - `w`, `h`, `fit` (resize)
  - `w`, `h`, `gravity` (crop)
  - `output`, `quality` (all)
- Two image source modes per tab:
  - **Use S3 key** — type an existing `bucket`/`key`.
  - **Upload** — pick a local file and PUT it to S3 directly from the browser via `@aws-sdk/client-s3`, then call imago with the freshly uploaded key.
- Live preview URL, copy-to-clipboard, download button.
- Result panel shows the rendered image + metadata: status, duration, content-type, byte size, pixel dimensions, and EXIF (parsed by `exifr`).
- Settings dialog stores imago base URL + S3 credentials in `localStorage`.

## Quick start

```bash
cd web-tool
npm install
npm run dev
```

Open http://localhost:5173, click **Settings**, then fill in:

- **imago base URL** — defaults to `/api/imago`, which is proxied by Vite to `http://localhost:8080`. Change to an absolute URL (e.g. `http://localhost:8080`) to hit the server directly — imago will need CORS headers in that case.
- **S3** — bucket, region, access key, secret, optional custom endpoint (for MinIO / R2), and a key prefix used for uploads.

## Scripts

| Command | Purpose |
|---------|---------|
| `npm run dev` | Start Vite dev server with imago proxy on `/api/imago`. |
| `npm run build` | Type-check (`tsc -b`) then produce a production build in `dist/`. |
| `npm run preview` | Preview the production build locally. |
| `npm run lint` | ESLint over `src/`. |

## Dev proxy vs. direct

`vite.config.ts` proxies `/api/imago/*` → `http://localhost:8080/*`. This sidesteps browser CORS during development. For a deployed build, either:

1. Host the static bundle from the same origin as imago, or
2. Add CORS to imago (Drogon exposes `registerAdvice` / `PreSendingAdvice` hooks) so the browser is allowed to call it cross-origin.

## Security note

S3 credentials live in `localStorage` and the browser calls AWS directly. Treat this tool as a **local dev aid**. If you share it with anyone else, issue IAM keys scoped to `s3:PutObject` on a single bucket/prefix — never reuse your personal keys. The S3 bucket also needs a CORS rule allowing `PUT` from the dev origin (e.g. `http://localhost:5173`).

Example S3 CORS rule:

```json
[
  {
    "AllowedOrigins": ["http://localhost:5173"],
    "AllowedMethods": ["PUT", "GET", "HEAD"],
    "AllowedHeaders": ["*"],
    "ExposeHeaders": ["ETag"]
  }
]
```

## Layout

```
src/
  App.tsx                 # tabs + header shell
  components/
    FeatureForm.tsx       # shared form; ResizeForm / CropForm / ConvertForm wrappers
    ResultPanel.tsx       # preview + metadata + EXIF drawer
    SourcePicker.tsx      # upload vs. S3-key toggle
    SettingsDialog.tsx    # persisted settings form
    ui/                   # shadcn-style Button, Card, Tabs, Select, Dialog, ...
  lib/
    imago.ts              # URL builder + fetch wrapper
    s3.ts                 # AWS SDK v3 PutObject helper
    metadata.ts           # dimension + EXIF extraction
    settings.ts           # localStorage-backed React context
    utils.ts              # cn(), formatBytes()
```
