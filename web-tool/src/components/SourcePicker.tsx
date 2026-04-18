import { useRef, useState } from 'react'
import { Upload, Key, Loader2 } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { uploadToS3 } from '@/lib/s3'
import { s3IsConfigured, useSettings } from '@/lib/settings'
import { formatBytes } from '@/lib/utils'

type Mode = 'upload' | 'key'

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
  const fileInput = useRef<HTMLInputElement>(null)

  const onFile = async (file: File) => {
    onChange({ ...value, pickedFile: file })
    if (!s3IsConfigured(settings)) {
      setError('Configure S3 credentials in Settings before uploading.')
      return
    }
    setError(null)
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

  return (
    <div className="grid gap-3">
      <div className="inline-flex w-fit rounded-md border border-(--color-border) bg-(--color-muted) p-0.5 text-xs">
        <ModeTab active={mode === 'key'} onClick={() => setMode('key')}>
          <Key className="h-3.5 w-3.5" /> Use S3 key
        </ModeTab>
        <ModeTab active={mode === 'upload'} onClick={() => setMode('upload')}>
          <Upload className="h-3.5 w-3.5" /> Upload
        </ModeTab>
      </div>

      <div className="grid grid-cols-[1fr_2fr] gap-3">
        <div className="grid gap-1">
          <Label>Bucket</Label>
          <Input
            value={value.bucket}
            placeholder={settings.s3Bucket || 'bucket'}
            onChange={(e) => onChange({ ...value, bucket: e.target.value })}
          />
        </div>
        <div className="grid gap-1">
          <Label>Object key</Label>
          <Input
            value={value.key}
            placeholder="path/to/image.jpg"
            onChange={(e) => onChange({ ...value, key: e.target.value })}
          />
        </div>
      </div>

      {mode === 'upload' && (
        <div className="grid gap-2 rounded-md border border-dashed border-(--color-border) p-4">
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
          <div className="flex items-center justify-between gap-3">
            <div className="text-sm">
              {value.pickedFile ? (
                <>
                  <span className="font-medium">{value.pickedFile.name}</span>
                  <span className="text-(--color-muted-foreground)">
                    {' '}
                    · {formatBytes(value.pickedFile.size)} · {value.pickedFile.type || 'unknown'}
                  </span>
                </>
              ) : (
                <span className="text-(--color-muted-foreground)">
                  Pick an image — it will be PUT to s3://{settings.s3Bucket || '<bucket>'}/
                  {settings.s3KeyPrefix}
                </span>
              )}
            </div>
            <Button
              size="sm"
              variant="outline"
              onClick={() => fileInput.current?.click()}
              disabled={uploading}
            >
              {uploading ? <Loader2 className="h-4 w-4 animate-spin" /> : <Upload className="h-4 w-4" />}
              {value.pickedFile ? 'Replace' : 'Choose file'}
            </Button>
          </div>
          {error && <p className="text-xs text-(--color-destructive)">{error}</p>}
          {value.pickedFile && value.key && !uploading && !error && (
            <p className="text-xs text-(--color-muted-foreground)">Uploaded → {value.key}</p>
          )}
        </div>
      )}
    </div>
  )
}

function ModeTab({
  active,
  onClick,
  children,
}: {
  active: boolean
  onClick: () => void
  children: React.ReactNode
}) {
  return (
    <button
      type="button"
      onClick={onClick}
      className={
        'inline-flex items-center gap-1 rounded px-3 py-1 transition-colors ' +
        (active
          ? 'bg-(--color-background) text-(--color-foreground) shadow'
          : 'text-(--color-muted-foreground) hover:text-(--color-foreground)')
      }
    >
      {children}
    </button>
  )
}
