import { useEffect, useRef, useState } from 'react'
import { Image as ImageIcon, Loader2 } from 'lucide-react'
import type { SourceState } from '@/components/SourcePicker'
import { ExifSummary } from '@/components/ExifSummary'
import { downloadFromS3 } from '@/lib/s3'
import { extractMetadata, type ImageMetadata } from '@/lib/metadata'
import { useSettings } from '@/lib/settings'
import { formatBytes } from '@/lib/utils'

interface Props {
  source: SourceState
}

interface Loaded {
  url: string
  meta: ImageMetadata
}

export function InputPreview({ source }: Props) {
  const { settings } = useSettings()
  const [loaded, setLoaded] = useState<Loaded | null>(null)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const lastSignature = useRef('')

  const bucket = source.bucket || settings.s3Bucket
  const key = source.key

  useEffect(() => {
    // Prefer the S3 key when set — that's what imago will actually process.
    // Fall back to the locally picked file (e.g. mid-upload, before we have
    // a key yet) so there's always something to preview.
    const signature =
      bucket && key
        ? `s3:${bucket}:${key}`
        : source.pickedFile
          ? `file:${source.pickedFile.name}:${source.pickedFile.size}:${source.pickedFile.lastModified}`
          : ''

    if (signature === lastSignature.current) return
    lastSignature.current = signature

    setError(null)
    setLoaded((prev) => {
      if (prev) URL.revokeObjectURL(prev.url)
      return null
    })

    if (!signature) return

    const debounce = setTimeout(() => {
      const cancel = { aborted: false }
      void (async () => {
        setLoading(true)
        try {
          const blob =
            bucket && key
              ? await downloadFromS3(settings, bucket, key)
              : (source.pickedFile as File)
          if (cancel.aborted) return
          const contentType = blob.type || 'application/octet-stream'
          const meta = await extractMetadata(blob, contentType)
          if (cancel.aborted) return
          const url = URL.createObjectURL(blob)
          setLoaded({ url, meta })
        } catch (e) {
          if (!cancel.aborted) {
            setError(e instanceof Error ? e.message : String(e))
          }
        } finally {
          if (!cancel.aborted) setLoading(false)
        }
      })()
      return () => {
        cancel.aborted = true
      }
    }, 250)

    return () => clearTimeout(debounce)
  }, [bucket, key, source.pickedFile, settings])

  // Release the last object URL when the component unmounts.
  useEffect(() => {
    return () => {
      if (loaded) URL.revokeObjectURL(loaded.url)
    }
  }, [loaded])

  const hasAnySource = Boolean((bucket && key) || source.pickedFile)
  if (!hasAnySource) return null

  return (
    <div className="grid gap-3 rounded-md border border-(--color-border) p-3">
      <div className="flex items-center justify-between">
        <span className="text-xs font-semibold uppercase tracking-wide text-(--color-muted-foreground)">
          Input preview
        </span>
        {loading && <Loader2 className="h-3.5 w-3.5 animate-spin text-(--color-muted-foreground)" />}
      </div>

      {error && !loading && <div className="text-xs text-(--color-destructive)">{error}</div>}

      {!loading && !error && !loaded && (
        <div className="flex h-24 items-center justify-center text-xs text-(--color-muted-foreground)">
          <ImageIcon className="mr-2 h-4 w-4" /> No preview available.
        </div>
      )}

      {loaded && (
        <div className="grid gap-3 md:grid-cols-[1fr_1fr]">
          <div className="overflow-hidden rounded-md bg-(--color-muted)">
            <img
              src={loaded.url}
              alt="input preview"
              className="max-h-[240px] w-full object-contain"
            />
          </div>
          <div className="grid gap-1 text-xs">
            <Row label="Source" value={bucket && key ? `s3://${bucket}/${key}` : 'local file'} />
            <Row label="Size" value={formatBytes(loaded.meta.bytes)} />
            <Row label="Content-Type" value={loaded.meta.contentType || '—'} />
            <Row
              label="Dimensions"
              value={
                loaded.meta.width && loaded.meta.height
                  ? `${loaded.meta.width} × ${loaded.meta.height}`
                  : '—'
              }
            />
          </div>
          <div className="md:col-span-2">
            <ExifSummary exif={loaded.meta.exif} compact />
          </div>
        </div>
      )}
    </div>
  )
}

function Row({ label, value }: { label: string; value: string }) {
  return (
    <div className="grid grid-cols-[auto_1fr] gap-2 border-b border-(--color-border) pb-1 last:border-0">
      <span className="text-(--color-muted-foreground)">{label}</span>
      <span className="truncate font-mono">{value}</span>
    </div>
  )
}
