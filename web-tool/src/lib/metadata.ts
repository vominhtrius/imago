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

// exifr doesn't support WebP containers (JPEG / TIFF / HEIC / PNG only). Pull
// the "EXIF" RIFF chunk ourselves and hand the raw TIFF bytes inside it to
// exifr — it parses TIFF directly. Returns null if this isn't a WebP or the
// file has no EXIF chunk.
function extractWebpExifTiff(buf: ArrayBuffer): Uint8Array | null {
  const u8 = new Uint8Array(buf)
  if (u8.length < 12) return null
  // Magic: "RIFF" .... "WEBP"
  if (u8[0] !== 0x52 || u8[1] !== 0x49 || u8[2] !== 0x46 || u8[3] !== 0x46) return null
  if (u8[8] !== 0x57 || u8[9] !== 0x45 || u8[10] !== 0x42 || u8[11] !== 0x50) return null

  const dv = new DataView(buf)
  let off = 12
  while (off + 8 <= u8.length) {
    const tag = String.fromCharCode(u8[off], u8[off + 1], u8[off + 2], u8[off + 3])
    const size = dv.getUint32(off + 4, true)
    const payload = off + 8
    if (tag === 'EXIF') {
      if (payload + size > u8.length) return null
      let start = payload
      let end = payload + size
      // libvips prefixes WebP EXIF payloads with JPEG's "Exif\0\0" marker.
      // Skip it so the remaining bytes start with the TIFF header (II/MM).
      if (
        end - start >= 6 &&
        u8[start] === 0x45 && u8[start + 1] === 0x78 &&
        u8[start + 2] === 0x69 && u8[start + 3] === 0x66 &&
        u8[start + 4] === 0x00 && u8[start + 5] === 0x00
      ) {
        start += 6
      }
      return u8.subarray(start, end)
    }
    // RIFF chunks are word-aligned — odd sizes get a pad byte.
    off = payload + size + (size & 1)
  }
  return null
}

export async function extractMetadata(blob: Blob, contentType: string): Promise<ImageMetadata> {
  const dims = await readImageDimensions(blob)
  let exif: Record<string, unknown> | undefined
  try {
    const buf = await blob.arrayBuffer()
    const tiff = extractWebpExifTiff(buf)
    const input = tiff ?? buf
    const parsed = await exifr.parse(input, { tiff: true, xmp: false, iptc: false, ifd1: false })
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
