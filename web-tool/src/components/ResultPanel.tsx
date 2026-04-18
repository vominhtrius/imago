import { useEffect, useState } from 'react'
import { Clock, Copy, Download, ExternalLink, Server } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { ExifSummary } from '@/components/ExifSummary'
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
          <>
            <TimingBar
              serverMs={result.serverMs}
              totalMs={result.durationMs}
            />
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
                <MetaRow label="Content-Type" value={result.contentType} />
                <MetaRow label="Size" value={meta ? formatBytes(meta.bytes) : formatBytes(result.blob.size)} />
                <MetaRow
                  label="Dimensions"
                  value={meta?.width && meta.height ? `${meta.width} × ${meta.height}` : '—'}
                />
                <ExifSummary exif={meta?.exif} />
              </div>
            </div>
          </>
        )}
      </CardContent>
    </Card>
  )
}

function TimingBar({
  serverMs,
  totalMs,
}: {
  serverMs: number | null
  totalMs: number
}) {
  // Network + client decode time = total − server. If the server didn't
  // emit Server-Timing we only know the total; show it as one pill.
  const server = serverMs ?? 0
  const network = Math.max(0, totalMs - server)
  return (
    <div className="grid gap-2 rounded-md border border-(--color-border) bg-(--color-muted)/50 p-3">
      <div className="flex items-center justify-between">
        <span className="text-xs font-semibold uppercase tracking-wide text-(--color-muted-foreground)">
          Timing
        </span>
        <span className="text-base font-semibold tabular-nums">
          {totalMs.toFixed(0)} ms
          <span className="ml-1 text-xs font-normal text-(--color-muted-foreground)">
            total
          </span>
        </span>
      </div>
      {serverMs !== null && (
        <>
          <div className="grid grid-cols-3 gap-2 text-xs">
            <Pill
              icon={<Server className="h-3.5 w-3.5" />}
              label="Server"
              value={`${server.toFixed(1)} ms`}
              hint="imago processing"
            />
            <Pill
              icon={<Clock className="h-3.5 w-3.5" />}
              label="Network"
              value={`${network.toFixed(1)} ms`}
              hint="transit + decode"
            />
            <Pill
              label="Share"
              value={`${((server / totalMs) * 100).toFixed(0)}%`}
              hint="server / total"
            />
          </div>
          <div
            className="flex h-1.5 overflow-hidden rounded-full bg-(--color-border)"
            role="img"
            aria-label={`server ${server.toFixed(1)}ms of ${totalMs.toFixed(1)}ms total`}
          >
            <div
              className="bg-(--color-primary)"
              style={{ width: `${Math.min(100, (server / totalMs) * 100)}%` }}
            />
          </div>
        </>
      )}
    </div>
  )
}

function Pill({
  icon,
  label,
  value,
  hint,
}: {
  icon?: React.ReactNode
  label: string
  value: string
  hint?: string
}) {
  return (
    <div
      className="grid gap-0.5 rounded-md border border-(--color-border) bg-(--color-background) p-2"
      title={hint}
    >
      <span className="inline-flex items-center gap-1 text-[10px] uppercase tracking-wide text-(--color-muted-foreground)">
        {icon}
        {label}
      </span>
      <span className="font-mono text-sm tabular-nums">{value}</span>
    </div>
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

