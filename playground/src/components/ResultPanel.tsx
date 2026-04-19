import { useEffect, useState } from 'react'
import { Clock, Copy, Download, ExternalLink, Server } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { ExifSummary } from '@/components/ExifSummary'
import type { FetchResult } from '@/lib/imago'
import { extractMetadata, type ImageMetadata } from '@/lib/metadata'
import { formatBytes } from '@/lib/utils'

export interface ServiceOutcome {
  label: string
  requestUrl: string
  result: FetchResult | null
  error: string | null
}

interface Props {
  outcomes: ServiceOutcome[]
  loading: boolean
}

export function ResultPanel({ outcomes, loading }: Props) {
  const anyRequest = outcomes.some((o) => o.requestUrl)
  const anyResult = outcomes.some((o) => o.result)
  const anyError = outcomes.some((o) => o.error)

  return (
    <Card className="h-full">
      <CardHeader>
        <CardTitle>Results</CardTitle>
        <p className="text-xs text-(--color-muted-foreground)">
          Side-by-side output from each service.
        </p>
      </CardHeader>
      <CardContent className="grid gap-4">
        {loading && (
          <div className="flex h-40 items-center justify-center rounded-md bg-(--color-muted) text-sm text-(--color-muted-foreground)">
            Requesting services…
          </div>
        )}

        {!loading && !anyRequest && (
          <div className="flex h-40 items-center justify-center rounded-md border border-dashed border-(--color-border) text-sm text-(--color-muted-foreground)">
            Submit the form to see results.
          </div>
        )}

        {anyRequest && (anyResult || anyError || loading) && (
          <div className="grid gap-4 xl:grid-cols-2">
            {outcomes.map((o) => (
              <ServiceCard key={o.label} outcome={o} loading={loading} />
            ))}
          </div>
        )}

        {!loading && anyResult && outcomes.length >= 2 && <ComparisonStrip outcomes={outcomes} />}
      </CardContent>
    </Card>
  )
}

function ServiceCard({ outcome, loading }: { outcome: ServiceOutcome; loading: boolean }) {
  const { label, requestUrl, result, error } = outcome
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
    <div className="grid gap-3 rounded-md border border-(--color-border) bg-(--color-card) p-3">
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2">
          <span className="rounded bg-(--color-primary)/10 px-1.5 py-0.5 text-xs font-semibold uppercase tracking-wide text-(--color-primary)">
            {label}
          </span>
          {result && (
            <span className="text-xs text-(--color-muted-foreground)">
              HTTP {result.status}
            </span>
          )}
        </div>
        {result && (
          <div className="flex gap-1">
            <Button variant="outline" size="sm" asChild>
              <a href={result.url} download={`${label}-result`}>
                <Download className="h-3.5 w-3.5" />
              </a>
            </Button>
            <Button variant="outline" size="sm" asChild>
              <a href={requestUrl} target="_blank" rel="noreferrer">
                <ExternalLink className="h-3.5 w-3.5" />
              </a>
            </Button>
          </div>
        )}
      </div>

      {requestUrl && (
        <div className="flex items-center gap-2 rounded-md bg-(--color-muted) p-2 font-mono text-[11px]">
          <code className="break-all">{requestUrl}</code>
          <Button
            variant="ghost"
            size="icon"
            className="h-6 w-6 shrink-0"
            onClick={() => navigator.clipboard.writeText(requestUrl)}
            title="Copy URL"
          >
            <Copy className="h-3 w-3" />
          </Button>
        </div>
      )}

      {error && (
        <div className="rounded-md border border-(--color-destructive) bg-(--color-destructive)/10 p-2 text-xs text-(--color-destructive)">
          {error}
        </div>
      )}

      {loading && !result && !error && (
        <div className="flex h-32 items-center justify-center rounded-md bg-(--color-muted) text-xs text-(--color-muted-foreground)">
          Requesting {label}…
        </div>
      )}

      {result && (
        <>
          <TimingBar serverMs={result.serverMs} totalMs={result.durationMs} />
          <div className="overflow-hidden rounded-md border border-(--color-border) bg-(--color-muted)">
            <img
              src={result.url}
              alt={`${label} result`}
              className="max-h-[380px] w-full object-contain"
            />
          </div>
          <div className="grid gap-1.5 text-xs">
            <MetaRow label="Content-Type" value={result.contentType} />
            <MetaRow
              label="Size"
              value={meta ? formatBytes(meta.bytes) : formatBytes(result.blob.size)}
            />
            <MetaRow
              label="Dimensions"
              value={meta?.width && meta.height ? `${meta.width} × ${meta.height}` : '—'}
            />
            <ExifSummary exif={meta?.exif} />
          </div>
        </>
      )}
    </div>
  )
}

function ComparisonStrip({ outcomes }: { outcomes: ServiceOutcome[] }) {
  const rows = outcomes
    .filter((o) => o.result)
    .map((o) => ({
      label: o.label,
      total: o.result!.durationMs,
      server: o.result!.serverMs,
      bytes: o.result!.blob.size,
    }))
  if (rows.length < 2) return null
  const fastestTotal = Math.min(...rows.map((r) => r.total))
  const smallestBytes = Math.min(...rows.map((r) => r.bytes))
  return (
    <div className="grid gap-2 rounded-md border border-(--color-border) bg-(--color-muted)/30 p-3 text-xs">
      <div className="font-semibold uppercase tracking-wide text-(--color-muted-foreground)">
        Comparison
      </div>
      <table className="w-full border-collapse text-left">
        <thead>
          <tr className="text-(--color-muted-foreground)">
            <th className="py-1 pr-2">Service</th>
            <th className="py-1 pr-2">Total</th>
            <th className="py-1 pr-2">Server</th>
            <th className="py-1 pr-2">Bytes</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((r) => (
            <tr key={r.label} className="border-t border-(--color-border)">
              <td className="py-1 pr-2 font-mono">{r.label}</td>
              <td className="py-1 pr-2 tabular-nums">
                {r.total.toFixed(0)} ms
                {r.total === fastestTotal && rows.length > 1 && (
                  <span className="ml-1 text-(--color-primary)">fastest</span>
                )}
              </td>
              <td className="py-1 pr-2 tabular-nums">
                {r.server != null ? `${r.server.toFixed(1)} ms` : '—'}
              </td>
              <td className="py-1 pr-2 tabular-nums">
                {formatBytes(r.bytes)}
                {r.bytes === smallestBytes && rows.length > 1 && (
                  <span className="ml-1 text-(--color-primary)">smallest</span>
                )}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

function TimingBar({
  serverMs,
  totalMs,
}: {
  serverMs: number | null
  totalMs: number
}) {
  // Services that don't emit Server-Timing (e.g. imgproxy) render the pills
  // with em-dashes — keeps the card shape identical to imago so the two
  // panels line up side-by-side.
  const hasServer = serverMs !== null
  const server = serverMs ?? 0
  const network = hasServer ? Math.max(0, totalMs - server) : null
  const share = hasServer && totalMs > 0 ? (server / totalMs) * 100 : null

  return (
    <div className="grid gap-2 rounded-md border border-(--color-border) bg-(--color-muted)/50 p-2">
      <div className="flex items-center justify-between">
        <span className="text-[10px] font-semibold uppercase tracking-wide text-(--color-muted-foreground)">
          Timing
        </span>
        <span className="text-sm font-semibold tabular-nums">
          {totalMs.toFixed(0)} ms
          <span className="ml-1 text-[10px] font-normal text-(--color-muted-foreground)">
            total
          </span>
        </span>
      </div>
      <div className="grid grid-cols-3 gap-1.5 text-[11px]">
        <Pill
          icon={<Server className="h-3 w-3" />}
          label="Server"
          value={hasServer ? `${server.toFixed(1)} ms` : '—'}
          hint={hasServer ? 'processing' : 'Server-Timing header not emitted'}
        />
        <Pill
          icon={<Clock className="h-3 w-3" />}
          label="Network"
          value={network !== null ? `${network.toFixed(1)} ms` : '—'}
          hint={network !== null ? 'transit + decode' : 'requires Server-Timing'}
        />
        <Pill
          label="Share"
          value={share !== null ? `${share.toFixed(0)}%` : '—'}
          hint={share !== null ? 'server / total' : 'requires Server-Timing'}
        />
      </div>
      <div
        className="flex h-1 overflow-hidden rounded-full bg-(--color-border)"
        role="img"
        aria-label={
          hasServer
            ? `server ${server.toFixed(1)}ms of ${totalMs.toFixed(1)}ms total`
            : `total ${totalMs.toFixed(1)}ms (no server timing)`
        }
      >
        {share !== null && (
          <div
            className="bg-(--color-primary)"
            style={{ width: `${Math.min(100, share)}%` }}
          />
        )}
      </div>
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
      className="grid gap-0.5 rounded-md border border-(--color-border) bg-(--color-background) p-1.5"
      title={hint}
    >
      <span className="inline-flex items-center gap-1 text-[9px] uppercase tracking-wide text-(--color-muted-foreground)">
        {icon}
        {label}
      </span>
      <span className="font-mono text-xs tabular-nums">{value}</span>
    </div>
  )
}

function MetaRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="grid grid-cols-[auto_1fr] gap-2 border-b border-(--color-border) pb-1 last:border-0">
      <span className="text-(--color-muted-foreground)">{label}</span>
      <span className="truncate font-mono text-[11px]">{value}</span>
    </div>
  )
}
