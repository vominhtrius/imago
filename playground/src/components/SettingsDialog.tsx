import { useRef, useState } from 'react'
import { Settings, Download, Upload } from 'lucide-react'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
  DialogTrigger,
} from '@/components/ui/dialog'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import {
  DEFAULT_SETTINGS,
  parseSettingsJson,
  settingsToJson,
  useSettings,
  type AppSettings,
} from '@/lib/settings'

export function SettingsDialog() {
  const { settings, replace } = useSettings()
  const [open, setOpen] = useState(false)
  const [draft, setDraft] = useState<AppSettings>(settings)
  const [importError, setImportError] = useState<string | null>(null)
  const fileInputRef = useRef<HTMLInputElement>(null)

  const onOpenChange = (next: boolean) => {
    if (next) {
      setDraft(settings)
      setImportError(null)
    }
    setOpen(next)
  }

  const onSave = () => {
    replace(draft)
    setOpen(false)
  }

  const onReset = () => setDraft(DEFAULT_SETTINGS)

  const set = (patch: Partial<AppSettings>) => setDraft((d) => ({ ...d, ...patch }))

  const onExport = () => {
    const blob = new Blob([settingsToJson(draft)], { type: 'application/json' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    const stamp = new Date().toISOString().replace(/[:.]/g, '-')
    a.download = `imago-playground-settings-${stamp}.json`
    document.body.appendChild(a)
    a.click()
    document.body.removeChild(a)
    URL.revokeObjectURL(url)
  }

  const onImportClick = () => {
    setImportError(null)
    fileInputRef.current?.click()
  }

  const onImportFile = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0]
    // Reset so selecting the same file again re-fires the change event.
    e.target.value = ''
    if (!file) return
    try {
      const text = await file.text()
      setDraft(parseSettingsJson(text))
      setImportError(null)
    } catch (err) {
      setImportError(err instanceof Error ? err.message : String(err))
    }
  }

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogTrigger asChild>
        <Button variant="outline" size="sm">
          <Settings className="h-4 w-4" />
          Settings
        </Button>
      </DialogTrigger>
      <DialogContent className="sm:max-w-xl">
        <DialogHeader>
          <DialogTitle>Connection settings</DialogTitle>
          <DialogDescription>
            Stored locally in your browser. S3 credentials are used directly from the browser via the
            AWS SDK — only use keys restricted to the uploads bucket.
          </DialogDescription>
        </DialogHeader>

        <div className="grid gap-4 py-2">
          <section className="grid gap-2">
            <h4 className="text-sm font-semibold">Services</h4>
            <Field label="imago base URL" hint="e.g. /api/imago (dev proxy) or http://localhost:8080">
              <Input
                value={draft.imagoBaseUrl}
                onChange={(e) => set({ imagoBaseUrl: e.target.value })}
                placeholder="/api/imago"
              />
            </Field>
            <Field
              label="imgproxy base URL"
              hint="Leave blank to disable side-by-side comparison. e.g. /api/imgproxy or http://localhost:8082"
            >
              <Input
                value={draft.imgproxyBaseUrl}
                onChange={(e) => set({ imgproxyBaseUrl: e.target.value })}
                placeholder="/api/imgproxy"
              />
            </Field>
          </section>

          <section className="grid gap-2">
            <h4 className="text-sm font-semibold">S3 / storage</h4>
            <div className="grid grid-cols-2 gap-3">
              <Field label="Bucket">
                <Input
                  value={draft.s3Bucket}
                  onChange={(e) => set({ s3Bucket: e.target.value })}
                  placeholder="my-bucket"
                />
              </Field>
              <Field label="Region">
                <Input
                  value={draft.s3Region}
                  onChange={(e) => set({ s3Region: e.target.value })}
                  placeholder="us-east-1"
                />
              </Field>
              <Field label="Access key ID">
                <Input
                  value={draft.s3AccessKeyId}
                  onChange={(e) => set({ s3AccessKeyId: e.target.value })}
                  placeholder="AKIA…"
                />
              </Field>
              <Field label="Secret access key">
                <Input
                  type="password"
                  value={draft.s3SecretAccessKey}
                  onChange={(e) => set({ s3SecretAccessKey: e.target.value })}
                  placeholder="••••••••"
                />
              </Field>
              <Field label="Custom endpoint" hint="Optional — e.g. MinIO / R2">
                <Input
                  value={draft.s3Endpoint}
                  onChange={(e) => set({ s3Endpoint: e.target.value })}
                  placeholder="https://s3.example.com"
                />
              </Field>
              <Field label="Key prefix" hint="Applied to generated upload keys">
                <Input
                  value={draft.s3KeyPrefix}
                  onChange={(e) => set({ s3KeyPrefix: e.target.value })}
                  placeholder="uploads/"
                />
              </Field>
            </div>

            <label className="mt-1 flex items-center gap-2 text-sm">
              <input
                type="checkbox"
                checked={draft.s3ForcePathStyle}
                onChange={(e) => set({ s3ForcePathStyle: e.target.checked })}
              />
              Force path-style URLs (required for MinIO and most non-AWS providers)
            </label>
          </section>
        </div>

        {importError && (
          <p className="text-xs text-(--color-destructive)">Import failed: {importError}</p>
        )}

        <DialogFooter className="flex-wrap gap-2 sm:justify-between">
          <div className="flex gap-2">
            <Button variant="outline" size="sm" onClick={onImportClick}>
              <Upload className="h-4 w-4" />
              Import
            </Button>
            <Button variant="outline" size="sm" onClick={onExport}>
              <Download className="h-4 w-4" />
              Export
            </Button>
            <input
              ref={fileInputRef}
              type="file"
              accept="application/json,.json"
              onChange={onImportFile}
              className="hidden"
            />
          </div>
          <div className="flex gap-2">
            <Button variant="ghost" onClick={onReset}>
              Reset
            </Button>
            <Button onClick={onSave}>Save</Button>
          </div>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}

function Field({
  label,
  hint,
  children,
}: {
  label: string
  hint?: string
  children: React.ReactNode
}) {
  return (
    <div className="grid gap-1">
      <Label>{label}</Label>
      {children}
      {hint ? <p className="text-xs text-(--color-muted-foreground)">{hint}</p> : null}
    </div>
  )
}
