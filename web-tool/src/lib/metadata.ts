import exifr from 'exifr'

export interface ImageMetadata {
  width: number | null
  height: number | null
  bytes: number
  contentType: string
  exif?: Record<string, unknown>
}

export async function readImageDimensions(blob: Blob): Promise<{ width: number; height: number } | null> {
  const url = URL.createObjectURL(blob)
  try {
    return await new Promise((resolve) => {
      const img = new Image()
      img.onload = () => resolve({ width: img.naturalWidth, height: img.naturalHeight })
      img.onerror = () => resolve(null)
      img.src = url
    })
  } finally {
    // revoke after a tick so the <img> has time to load
    setTimeout(() => URL.revokeObjectURL(url), 2000)
  }
}

export async function extractMetadata(blob: Blob, contentType: string): Promise<ImageMetadata> {
  const dims = await readImageDimensions(blob)
  let exif: Record<string, unknown> | undefined
  try {
    const buf = await blob.arrayBuffer()
    const parsed = await exifr.parse(buf, { tiff: true, xmp: false, iptc: false, ifd1: false })
    if (parsed && typeof parsed === 'object') {
      exif = parsed as Record<string, unknown>
    }
  } catch {
    exif = undefined
  }
  return {
    width: dims?.width ?? null,
    height: dims?.height ?? null,
    bytes: blob.size,
    contentType,
    exif,
  }
}

export function buildExifRows(
  exif: Record<string, unknown> | undefined,
): Array<[string, string]> {
  if (!exif) return []
  const out: Array<[string, string]> = []
  for (const [k, v] of Object.entries(exif)) {
    if (v === null || v === undefined) continue
    let value: string
    if (v instanceof Date) value = v.toISOString()
    else if (typeof v === 'object') {
      try {
        value = JSON.stringify(v)
      } catch {
        value = String(v)
      }
    } else {
      value = String(v)
    }
    if (value.length > 120) value = value.slice(0, 120) + '…'
    out.push([k, value])
  }
  return out
}
