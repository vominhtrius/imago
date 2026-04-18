import { useEffect, useMemo, useState } from 'react'
import { Copy, Download, ExternalLink } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import type { FetchResult } from '@/lib/imago'
import { extractMetadata, type ImageMetadata } from '@/lib/metadata'
import { formatBytes } from '@/lib/utils'

interface Props {
  result: FetchResult | null
  requestUrl: string
  error: string | null
  loading: boolean
}

export function ResultPanel({ result, requestUrl, error, loading }: Props) {
  const [meta, setMeta] = useState<ImageMetadata | null>(null)

  useEffect(() => {
    if (!result) {
      setMeta(null)
      return
    }
    let cancelled = false
    extractMetadata(result.blob, result.contentType).then((m) => {
      if (!cancelled) setMeta(m)
    })
    return () => {
      cancelled = true
    }
  }, [result])

  useEffect(() => {
    return () => {
      if (result?.url) URL.revokeObjectURL(result.url)
    }
  }, [result])

  const exifRows = useMemo(() => buildExifRows(meta?.exif), [meta])

  return (
    <Card className="h-full">
      <CardHeader className="flex flex-row items-center justify-between">
        <CardTitle>Result</CardTitle>
        {result && (
          <div className="flex gap-2">
            <Button variant="outline" size="sm" asChild>
              <a href={result.url} download="imago-result">
                <Download className="h-4 w-4" /> Download
              </a>
            </Button>
            <Button variant="outline" size="sm" asChild>
              <a href={requestUrl} target="_blank" rel="noreferrer">
                <ExternalLink className="h-4 w-4" /> Open
              </a>
            </Button>
          </div>
        )}
      </CardHeader>
      <CardContent className="grid gap-4">
        {requestUrl && (
          <div className="grid gap-1">
            <span className="text-xs font-medium uppercase tracking-wide text-(--color-muted-foreground)">
              Request
            </span>
            <div className="flex items-center gap-2 rounded-md bg-(--color-muted) p-2 font-mono text-xs">
              <code className="break-all">{requestUrl}</code>
              <Button
                variant="ghost"
                size="icon"
                className="h-7 w-7"
                onClick={() => navigator.clipboard.writeText(requestUrl)}
                title="Copy URL"
              >
                <Copy className="h-3.5 w-3.5" />
              </Button>
            </div>
          </div>
        )}

        {error && (
          <div className="rounded-md border border-(--color-destructive) bg-(--color-destructive)/10 p-3 text-sm text-(--color-destructive)">
            {error}
          </div>
        )}

        {loading && (
          <div className="flex h-40 items-center justify-center rounded-md bg-(--color-muted) text-sm text-(--color-muted-foreground)">
            Requesting imago…
          </div>
        )}

        {!loading && !result && !error && (
          <div className="flex h-40 items-center justify-center rounded-md border border-dashed border-(--color-border) text-sm text-(--color-muted-foreground)">
            Submit the form to see a preview.
          </div>
        )}

        {result && (
          <div className="grid gap-3 md:grid-cols-[2fr_1fr]">
            <div className="overflow-hidden rounded-md border border-(--color-border) bg-(--color-muted)">
              <img
                src={result.url}
                alt="imago result"
                className="max-h-[520px] w-full object-contain"
              />
            </div>
            <div className="grid gap-2 text-sm">
              <MetaRow label="Status" value={String(result.status)} />
              <MetaRow label="Duration" value={`${result.durationMs.toFixed(0)} ms`} />
              <MetaRow label="Content-Type" value={result.contentType} />
              <MetaRow label="Size" value={meta ? formatBytes(meta.bytes) : formatBytes(result.blob.size)} />
              <MetaRow
                label="Dimensions"
                value={meta?.width && meta.height ? `${meta.width} × ${meta.height}` : '—'}
              />
              {exifRows.length > 0 && (
                <details className="rounded-md border border-(--color-border) p-2 text-xs">
                  <summary className="cursor-pointer select-none font-medium">EXIF ({exifRows.length})</summary>
                  <div className="mt-2 grid gap-1">
                    {exifRows.map(([k, v]) => (
                      <div key={k} className="grid grid-cols-[auto_1fr] gap-2">
                        <span className="text-(--color-muted-foreground)">{k}</span>
                        <span className="truncate font-mono">{v}</span>
                      </div>
                    ))}
                  </div>
                </details>
              )}
            </div>
          </div>
        )}
      </CardContent>
    </Card>
  )
}

function MetaRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="grid grid-cols-[auto_1fr] gap-3 border-b border-(--color-border) pb-1 last:border-0">
      <span className="text-(--color-muted-foreground)">{label}</span>
      <span className="truncate font-mono text-xs">{value}</span>
    </div>
  )
}

function buildExifRows(exif: Record<string, unknown> | undefined): Array<[string, string]> {
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
