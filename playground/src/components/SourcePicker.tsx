import { useRef, useState } from 'react'
import { Upload, Key, Loader2, Zap, FolderOpen } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { uploadToS3 } from '@/lib/s3'
import { uploadToImago, type UploadResponse } from '@/lib/imago'
import { s3IsConfigured, useSettings } from '@/lib/settings'
import { formatBytes } from '@/lib/utils'
import { S3Browser } from '@/components/S3Browser'

type Mode = 'browse' | 'key' | 'upload-s3' | 'upload-imago'

export interface SourceState {
  bucket: string
  key: string
  pickedFile: File | null
}

interface Props {
  value: SourceState
  onChange: (s: SourceState) => void
}

export function SourcePicker({ value, onChange }: Props) {
  const { settings } = useSettings()
  const [mode, setMode] = useState<Mode>('key')
  const [uploading, setUploading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [imagoMeta, setImagoMeta] = useState<UploadResponse | null>(null)
  const fileInput = useRef<HTMLInputElement>(null)

  const onFile = async (file: File) => {
    onChange({ ...value, pickedFile: file })
    setError(null)
    setImagoMeta(null)

    if (mode === 'upload-imago') {
      setUploading(true)
      try {
        const res = await uploadToImago(settings, file, {
          bucket: settings.s3Bucket || undefined,
          prefix: settings.s3KeyPrefix || undefined,
        })
        onChange({ bucket: res.bucket, key: res.key, pickedFile: file })
        setImagoMeta(res)
      } catch (e) {
        setError(e instanceof Error ? e.message : 'imago upload failed')
      } finally {
        setUploading(false)
      }
      return
    }

    // upload-s3 (direct browser → S3) — requires configured credentials.
    if (!s3IsConfigured(settings)) {
      setError('Configure S3 credentials in Settings before uploading.')
      return
    }
    setUploading(true)
    try {
      const key = await uploadToS3(settings, file)
      onChange({ bucket: settings.s3Bucket, key, pickedFile: file })
    } catch (e) {
      setError(e instanceof Error ? e.message : 'upload failed')
    } finally {
      setUploading(false)
    }
  }

  const showUploadPanel = mode === 'upload-s3' || mode === 'upload-imago'
  const viaImago = mode === 'upload-imago'
  const targetHint = viaImago
    ? 'imago will re-encode + strip metadata'
    : `→ s3://${settings.s3Bucket || '<bucket>'}/${settings.s3KeyPrefix}`

  const setModeReset = (m: Mode) => {
    setMode(m)
    setError(null)
  }

  return (
    <div className="grid min-w-0 gap-2">
      <div className="grid w-full min-w-0 grid-cols-4 gap-0.5 rounded-md border border-(--color-border) bg-(--color-muted) p-0.5 text-xs">
        <ModeTab active={mode === 'browse'} onClick={() => setModeReset('browse')} title="Browse S3">
          <FolderOpen className="h-3.5 w-3.5 shrink-0" />
          <span className="truncate">Browse</span>
        </ModeTab>
        <ModeTab active={mode === 'key'} onClick={() => setModeReset('key')} title="Enter S3 key manually">
          <Key className="h-3.5 w-3.5 shrink-0" />
          <span className="truncate">Key</span>
        </ModeTab>
        <ModeTab
          active={mode === 'upload-imago'}
          onClick={() => setModeReset('upload-imago')}
          title="Upload via imago (re-encodes + strips metadata)"
        >
          <Zap className="h-3.5 w-3.5 shrink-0" />
          <span className="truncate">imago</span>
        </ModeTab>
        <ModeTab
          active={mode === 'upload-s3'}
          onClick={() => setModeReset('upload-s3')}
          title="Upload directly to S3"
        >
          <Upload className="h-3.5 w-3.5 shrink-0" />
          <span className="truncate">S3</span>
        </ModeTab>
      </div>

      {mode === 'browse' && (
        <S3Browser
          bucket={value.bucket || settings.s3Bucket}
          onBucketChange={(b) => onChange({ ...value, bucket: b })}
          onPick={(key) =>
            onChange({
              bucket: value.bucket || settings.s3Bucket,
              key,
              pickedFile: null,
            })
          }
          selectedKey={value.key}
        />
      )}

      {mode === 'key' && (
        <div className="grid grid-cols-[1fr_2fr] gap-2">
          <div className="grid gap-1">
            <Label className="text-xs">Bucket</Label>
            <Input
              value={value.bucket}
              placeholder={settings.s3Bucket || 'bucket'}
              onChange={(e) => onChange({ ...value, bucket: e.target.value })}
            />
          </div>
          <div className="grid gap-1">
            <Label className="text-xs">Object key</Label>
            <Input
              value={value.key}
              placeholder="path/to/image.jpg"
              onChange={(e) => onChange({ ...value, key: e.target.value })}
            />
          </div>
        </div>
      )}

      {showUploadPanel && (
        <div className="grid min-w-0 gap-1.5 overflow-hidden rounded-md border border-dashed border-(--color-border) p-2.5">
          <input
            ref={fileInput}
            type="file"
            accept="image/*"
            className="hidden"
            onChange={(e) => {
              const f = e.target.files?.[0]
              if (f) void onFile(f)
            }}
          />
          <div className="flex items-center justify-between gap-2">
            <div className="min-w-0 truncate text-xs">
              {value.pickedFile ? (
                <>
                  <span className="font-medium">{value.pickedFile.name}</span>
                  <span className="text-(--color-muted-foreground)">
                    {' '}
                    · {formatBytes(value.pickedFile.size)}
                  </span>
                </>
              ) : (
                <span className="text-(--color-muted-foreground)">{targetHint}</span>
              )}
            </div>
            <Button
              size="sm"
              variant="outline"
              onClick={() => fileInput.current?.click()}
              disabled={uploading}
              className="h-7 shrink-0 text-xs"
            >
              {uploading ? (
                <Loader2 className="h-3.5 w-3.5 animate-spin" />
              ) : (
                <Upload className="h-3.5 w-3.5" />
              )}
              {value.pickedFile ? 'Replace' : 'Choose file'}
            </Button>
          </div>
          {error && (
            <p className="break-all text-xs text-(--color-destructive)">{error}</p>
          )}
          {imagoMeta && !uploading && !error && viaImago && (
            <p className="break-all text-[11px] text-(--color-muted-foreground)">
              → <code className="font-mono">{imagoMeta.key}</code>
              <span className="whitespace-nowrap">
                {' '}· {imagoMeta.width}×{imagoMeta.height} ·{' '}
                {formatBytes(imagoMeta.bytes)}
              </span>
            </p>
          )}
          {!viaImago && value.pickedFile && value.key && !uploading && !error && (
            <p className="break-all text-[11px] text-(--color-muted-foreground)">
              → {value.key}
            </p>
          )}
        </div>
      )}
    </div>
  )
}

function ModeTab({
  active,
  onClick,
  title,
  children,
}: {
  active: boolean
  onClick: () => void
  title?: string
  children: React.ReactNode
}) {
  return (
    <button
      type="button"
      onClick={onClick}
      title={title}
      className={
        'inline-flex min-w-0 items-center justify-center gap-1 rounded px-1.5 py-1 transition-colors ' +
        (active
          ? 'bg-(--color-primary)/15 text-(--color-primary) shadow-sm font-medium'
          : 'text-(--color-muted-foreground) hover:text-(--color-foreground)')
      }
    >
      {children}
    </button>
  )
}
