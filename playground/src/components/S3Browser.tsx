import { useCallback, useEffect, useState } from 'react'
import { ChevronLeft, FileImage, Folder, Loader2, RefreshCw } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { listS3Objects, type S3Object } from '@/lib/s3'
import { s3IsConfigured, useSettings } from '@/lib/settings'
import { formatBytes } from '@/lib/utils'

interface Props {
  bucket: string
  onBucketChange: (b: string) => void
  onPick: (key: string) => void
  selectedKey: string
}

export function S3Browser({ bucket, onBucketChange, onPick, selectedKey }: Props) {
  const { settings } = useSettings()
  const [prefix, setPrefix] = useState(settings.s3KeyPrefix || '')
  const [objects, setObjects] = useState<S3Object[]>([])
  const [folders, setFolders] = useState<string[]>([])
  const [nextToken, setNextToken] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const load = useCallback(
    async (p: string, token?: string, append = false) => {
      if (!bucket || !s3IsConfigured(settings)) {
        setError('Configure S3 credentials and bucket in Settings.')
        setObjects([])
        setFolders([])
        setNextToken(null)
        return
      }
      setError(null)
      setLoading(true)
      try {
        const res = await listS3Objects(settings, bucket, p, token)
        setObjects((prev) => (append ? [...prev, ...res.objects] : res.objects))
        setFolders((prev) => (append ? [...prev, ...res.prefixes] : res.prefixes))
        setNextToken(res.nextToken)
      } catch (e) {
        setError(e instanceof Error ? e.message : 'list failed')
        if (!append) {
          setObjects([])
          setFolders([])
          setNextToken(null)
        }
      } finally {
        setLoading(false)
      }
    },
    [bucket, settings],
  )

  useEffect(() => {
    void load(prefix)
  }, [bucket, prefix, load])

  const goUp = () => {
    if (!prefix) return
    const trimmed = prefix.replace(/\/+$/, '')
    const parent = trimmed.includes('/') ? trimmed.slice(0, trimmed.lastIndexOf('/') + 1) : ''
    setPrefix(parent)
  }

  const isImage = (key: string) =>
    /\.(jpe?g|png|gif|webp|avif|heic|heif|tiff?|bmp|svg)$/i.test(key)

  // Segmented breadcrumb — each click jumps to that prefix depth.
  const segments = prefix.replace(/\/+$/, '').split('/').filter(Boolean)

  return (
    <div className="grid gap-2">
      {/* Single compact row: bucket field + breadcrumb + up/refresh */}
      <div className="flex items-center gap-1 rounded-md border border-(--color-border) bg-(--color-muted)/40 px-2 py-1 text-xs">
        <Input
          value={bucket}
          placeholder={settings.s3Bucket || 'bucket'}
          onChange={(e) => onBucketChange(e.target.value)}
          className="h-6 w-28 border-0 bg-transparent px-1 text-xs font-mono shadow-none focus-visible:ring-1"
        />
        <span className="text-(--color-muted-foreground)">/</span>
        <button
          type="button"
          onClick={() => setPrefix('')}
          className="font-mono text-(--color-muted-foreground) hover:text-(--color-foreground)"
          title="Root"
        >
          ~
        </button>
        {segments.map((seg, i) => (
          <span key={i} className="flex items-center gap-1">
            <span className="text-(--color-muted-foreground)">/</span>
            <button
              type="button"
              onClick={() => setPrefix(segments.slice(0, i + 1).join('/') + '/')}
              className="font-mono hover:text-(--color-primary)"
            >
              {seg}
            </button>
          </span>
        ))}
        <div className="ml-auto flex items-center gap-0.5">
          <Button
            size="icon"
            variant="ghost"
            onClick={goUp}
            disabled={!prefix || loading}
            className="h-6 w-6"
            aria-label="Up"
          >
            <ChevronLeft className="h-3.5 w-3.5" />
          </Button>
          <Button
            size="icon"
            variant="ghost"
            onClick={() => void load(prefix)}
            disabled={loading || !bucket}
            className="h-6 w-6"
            aria-label="Refresh"
          >
            {loading ? (
              <Loader2 className="h-3.5 w-3.5 animate-spin" />
            ) : (
              <RefreshCw className="h-3.5 w-3.5" />
            )}
          </Button>
        </div>
      </div>

      {error && <p className="text-xs text-(--color-destructive)">{error}</p>}

      <div className="max-h-48 overflow-y-auto rounded border border-(--color-border) bg-(--color-muted)/20">
        {folders.length === 0 && objects.length === 0 && !loading && !error && (
          <p className="p-2 text-xs text-(--color-muted-foreground)">No objects here.</p>
        )}
        <ul className="divide-y divide-(--color-border)/40 text-xs">
          {folders.map((f) => (
            <li key={`d:${f}`}>
              <button
                type="button"
                onClick={() => setPrefix(f)}
                className="flex w-full items-center gap-2 px-2 py-1 text-left hover:bg-(--color-muted)"
              >
                <Folder className="h-3.5 w-3.5 text-(--color-primary)" />
                <span className="font-mono">{f.slice(prefix.length)}</span>
              </button>
            </li>
          ))}
          {objects.map((o) => {
            const name = o.key.slice(prefix.length)
            const active = o.key === selectedKey
            return (
              <li key={`f:${o.key}`}>
                <button
                  type="button"
                  onClick={() => onPick(o.key)}
                  className={
                    'flex w-full items-center gap-2 px-2 py-1 text-left hover:bg-(--color-muted) ' +
                    (active ? 'bg-(--color-primary)/10' : '')
                  }
                  title={o.key}
                >
                  <FileImage
                    className={
                      'h-3.5 w-3.5 shrink-0 ' +
                      (isImage(o.key)
                        ? 'text-(--color-foreground)'
                        : 'text-(--color-muted-foreground)')
                    }
                  />
                  <span className="flex-1 truncate font-mono">{name}</span>
                  <span className="shrink-0 text-[10px] text-(--color-muted-foreground)">
                    {formatBytes(o.size)}
                  </span>
                </button>
              </li>
            )
          })}
        </ul>
      </div>

      {nextToken && (
        <Button
          size="sm"
          variant="ghost"
          onClick={() => void load(prefix, nextToken, true)}
          disabled={loading}
          className="h-6 text-xs"
        >
          {loading ? <Loader2 className="h-3 w-3 animate-spin" /> : null}
          Load more
        </Button>
      )}
    </div>
  )
}
