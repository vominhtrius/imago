import type { AppSettings } from './settings'

export type Operation = 'resize' | 'crop' | 'convert'

export interface BaseParams {
  output?: string
  quality?: number
}

export interface ResizeParams extends BaseParams {
  w?: number
  h?: number
  fit?: string
}

export interface CropParams extends BaseParams {
  w?: number
  h?: number
  gravity?: string
}

export type ConvertParams = BaseParams

function appendParams(url: URL, params: Record<string, unknown>) {
  for (const [k, v] of Object.entries(params)) {
    if (v === undefined || v === null || v === '') continue
    url.searchParams.set(k, String(v))
  }
}

function resolveFinal(base: string, url: URL): string {
  const isAbsolute = /^https?:/i.test(base)
  return isAbsolute ? url.toString() : `${url.pathname}${url.search}`
}

export function buildImagoUrl(
  settings: AppSettings,
  op: Operation,
  bucket: string,
  key: string,
  params: Record<string, unknown>,
): string {
  const base = settings.imagoBaseUrl.replace(/\/+$/, '')
  const safeKey = key.split('/').map(encodeURIComponent).join('/')
  const url = new URL(
    `${base}/${op}/${encodeURIComponent(bucket)}/${safeKey}`,
    window.location.origin,
  )
  appendParams(url, params)
  return resolveFinal(settings.imagoBaseUrl, url)
}

// imago-only gravity tokens — imgproxy rejects these with 404 "Invalid URL"
// so we skip the imgproxy leg entirely for those submits.
const IMGPROXY_UNSUPPORTED_GRAVITIES = new Set(['entropy'])

export interface ImgproxyUrlResult {
  url: string | null
  /** Present when `url` is null — human-readable reason for skipping. */
  skipReason?: string
}

// imgproxy URL format (unsigned):
//   resize  → /unsafe/rs:fit:W:H[/g:GRAVITY]/f:EXT[/q:Q]/plain/s3://BUCKET/KEY
//   crop    → /unsafe/rs:fill:W:H[/g:GRAVITY]/f:EXT[/q:Q]/plain/s3://BUCKET/KEY
//   convert → /unsafe/f:EXT[/q:Q]/plain/s3://BUCKET/KEY
//
// Accepts the same param shape as buildImagoUrl; fit/gravity values are
// passed through verbatim (tokens already chosen to match imgproxy: fit,
// fill, fill-down, force; ce, no, so, ea, we, noea, …, sm, fp:x:y).
//
// Returns `{ url: null, skipReason }` when the requested params can't be
// expressed in imgproxy — the caller should skip its leg and render the
// reason beside the imgproxy panel.
export function buildImgproxyUrl(
  settings: AppSettings,
  op: Operation,
  bucket: string,
  key: string,
  params: Record<string, unknown>,
): ImgproxyUrlResult {
  const base = settings.imgproxyBaseUrl.replace(/\/+$/, '')

  const gravity = typeof params.gravity === 'string' ? params.gravity : ''
  if (op === 'crop' && IMGPROXY_UNSUPPORTED_GRAVITIES.has(gravity)) {
    return {
      url: null,
      skipReason: `imgproxy has no "${gravity}" gravity — imago-only extension`,
    }
  }

  const ext = typeof params.output === 'string' && params.output ? String(params.output) : null
  const q = params.quality !== undefined && params.quality !== null && params.quality !== ''
    ? Number(params.quality)
    : null

  const segments: string[] = ['unsafe']

  if (op === 'resize' || op === 'crop') {
    const w = params.w !== undefined && params.w !== '' ? Number(params.w) : 0
    const h = params.h !== undefined && params.h !== '' ? Number(params.h) : 0
    const fitMode = op === 'crop'
      ? 'fill'
      : typeof params.fit === 'string' && params.fit
        ? String(params.fit)
        : 'fit'
    segments.push(`rs:${fitMode}:${w}:${h}`)
    if (op === 'crop' && gravity) {
      segments.push(`g:${gravity}`)
    }
  }

  if (ext) segments.push(`f:${ext}`)
  if (q !== null && Number.isFinite(q)) segments.push(`q:${q}`)

  const safeKey = key.split('/').map(encodeURIComponent).join('/')
  const plain = `plain/s3://${encodeURIComponent(bucket)}/${safeKey}`
  segments.push(plain)

  const path = `${base}/${segments.join('/')}`
  const url = new URL(path, window.location.origin)
  return { url: resolveFinal(settings.imgproxyBaseUrl, url) }
}

export interface FetchResult {
  blob: Blob
  url: string
  contentType: string
  /** Total client-measured round-trip (request → response body loaded). */
  durationMs: number
  /** Server-side processing time parsed from the `Server-Timing` header. */
  serverMs: number | null
  status: number
}

// `Server-Timing: <name>;dur=12.34, …` — find the first entry whose name
// matches any of the provided candidates.
function parseServerTiming(header: string | null, names: string[]): number | null {
  if (!header) return null
  for (const part of header.split(',')) {
    const [name, ...params] = part.trim().split(';')
    if (!names.includes(name.trim())) continue
    for (const p of params) {
      const [k, v] = p.trim().split('=')
      if (k === 'dur' && v) {
        const n = Number(v)
        if (Number.isFinite(n)) return n
      }
    }
  }
  return null
}

export interface FetchOptions {
  /** Service label used in thrown errors and Server-Timing name lookup. */
  label: string
  /** Candidate Server-Timing names to parse. Defaults to [label]. */
  timingNames?: string[]
}

export async function fetchImage(url: string, opts: FetchOptions): Promise<FetchResult> {
  const started = performance.now()
  // `cache: 'no-store'` bans the browser HTTP cache from both reading and
  // writing this response. Side-by-side timing only means something when
  // every submit actually hits the service.
  const res = await fetch(url, {
    method: 'GET',
    cache: 'no-store',
    headers: { 'Cache-Control': 'no-cache' },
  })
  const contentType = res.headers.get('content-type') ?? 'application/octet-stream'

  if (!res.ok) {
    const text = await res.text().catch(() => '')
    throw new Error(`${opts.label} ${res.status}: ${text || res.statusText}`)
  }

  const serverMs = parseServerTiming(
    res.headers.get('server-timing'),
    opts.timingNames ?? [opts.label],
  )
  const blob = await res.blob()
  const objectUrl = URL.createObjectURL(blob)
  return {
    blob,
    url: objectUrl,
    contentType,
    durationMs: performance.now() - started,
    serverMs,
    status: res.status,
  }
}

// Back-compat helper for existing call sites.
export function fetchImago(url: string): Promise<FetchResult> {
  return fetchImage(url, { label: 'imago' })
}

export interface UploadResponse {
  bucket: string
  key: string
  url: string
  content_type: string
  bytes: number
  width: number
  height: number
}

export interface UploadImagoOptions {
  /** Override the server-configured bucket (maps to ?bucket=). */
  bucket?: string
  /** Override the server-configured key prefix (maps to ?prefix=). */
  prefix?: string
  /** Cap the longest edge in pixels before re-encoding. 0 disables. */
  preResize?: number
  /** Re-encode HEIC/HEIF to JPEG for broader compatibility. */
  normalizeHeic?: boolean
  /** Force output format (webp | jpeg | png | avif). */
  output?: string
  /** Quality 1-100. */
  quality?: number
}

// POST a File to the imago upload endpoint. The server sniffs magic bytes,
// strips EXIF/IPTC/XMP, optionally pre-resizes, and re-encodes — the returned
// {bucket, key} pair addresses the stored object and can be fed to
// /resize, /crop, /convert exactly like a manually-keyed S3 object.
export async function uploadToImago(
  settings: AppSettings,
  file: File,
  opts: UploadImagoOptions = {},
): Promise<UploadResponse> {
  const base = settings.imagoBaseUrl.replace(/\/+$/, '')
  const url = new URL(`${base}/v1/upload`, window.location.origin)
  if (opts.bucket) url.searchParams.set('bucket', opts.bucket)
  if (opts.prefix) url.searchParams.set('prefix', opts.prefix)
  if (opts.preResize !== undefined) {
    url.searchParams.set('pre_resize', String(opts.preResize))
  }
  if (opts.normalizeHeic !== undefined) {
    url.searchParams.set('normalize_heic', opts.normalizeHeic ? 'true' : 'false')
  }
  if (opts.output) url.searchParams.set('output', opts.output)
  if (opts.quality !== undefined) url.searchParams.set('quality', String(opts.quality))

  const form = new FormData()
  form.append('file', file, file.name)

  const target = resolveFinal(settings.imagoBaseUrl, url)
  const res = await fetch(target, { method: 'POST', body: form })
  if (!res.ok) {
    const text = await res.text().catch(() => '')
    throw new Error(`imago upload ${res.status}: ${text || res.statusText}`)
  }
  return (await res.json()) as UploadResponse
}
