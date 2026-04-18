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

export function buildImagoUrl(
  settings: AppSettings,
  op: Operation,
  bucket: string,
  key: string,
  params: Record<string, unknown>,
): string {
  const base = settings.imagoBaseUrl.replace(/\/+$/, '')
  const safeKey = key.split('/').map(encodeURIComponent).join('/')
  const url = new URL(`${base}/${op}/${encodeURIComponent(bucket)}/${safeKey}`, window.location.origin)
  appendParams(url, params)
  const isAbsolute = /^https?:/i.test(settings.imagoBaseUrl)
  return isAbsolute ? url.toString() : `${url.pathname}${url.search}`
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

// `Server-Timing: imago;dur=12.34, other;dur=…` — find the `imago` entry.
function parseServerTiming(header: string | null): number | null {
  if (!header) return null
  for (const part of header.split(',')) {
    const [name, ...params] = part.trim().split(';')
    if (name.trim() !== 'imago') continue
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

export async function fetchImago(url: string): Promise<FetchResult> {
  const started = performance.now()
  const res = await fetch(url, { method: 'GET' })
  const contentType = res.headers.get('content-type') ?? 'application/octet-stream'

  if (!res.ok) {
    const text = await res.text().catch(() => '')
    throw new Error(`imago ${res.status}: ${text || res.statusText}`)
  }

  const serverMs = parseServerTiming(res.headers.get('server-timing'))
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
