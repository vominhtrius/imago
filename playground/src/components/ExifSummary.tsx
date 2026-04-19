import { useMemo } from 'react'
import { Info } from 'lucide-react'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogHeader,
  DialogTitle,
  DialogTrigger,
} from '@/components/ui/dialog'
import { buildExifRows } from '@/lib/metadata'

// Ordered allowlist — first hits win. Date fields are de-duplicated so we
// don't burn multiple featured slots on the same timestamp.
const PRIORITY = [
  'Make',
  'Model',
  'LensModel',
  'DateTimeOriginal',
  'CreateDate',
  'DateTime',
  'ModifyDate',
  'ExposureTime',
  'FNumber',
  'ISO',
  'FocalLength',
  'Orientation',
  'ImageWidth',
  'ImageHeight',
  'Software',
  'Artist',
  'Copyright',
] as const

const DATE_KEYS = new Set(['DateTimeOriginal', 'CreateDate', 'DateTime', 'ModifyDate'])

function pickFeatured(rows: Array<[string, string]>, limit = 5): Array<[string, string]> {
  const byKey = new Map(rows)
  const out: Array<[string, string]> = []
  let pickedDate = false
  for (const k of PRIORITY) {
    const v = byKey.get(k)
    if (v === undefined) continue
    if (DATE_KEYS.has(k)) {
      if (pickedDate) continue
      pickedDate = true
    }
    out.push([k, v])
    if (out.length >= limit) break
  }
  if (out.length < limit) {
    const picked = new Set(out.map(([k]) => k))
    for (const [k, v] of rows) {
      if (picked.has(k)) continue
      out.push([k, v])
      if (out.length >= limit) break
    }
  }
  return out
}

interface Props {
  exif: Record<string, unknown> | undefined
  compact?: boolean
}

export function ExifSummary({ exif, compact = false }: Props) {
  const allRows = useMemo(() => buildExifRows(exif), [exif])
  const featured = useMemo(() => pickFeatured(allRows), [allRows])

  if (allRows.length === 0) return null

  const labelSize = compact ? 'text-[11px]' : 'text-xs'
  const valueSize = compact ? 'text-[11px]' : 'text-xs'

  return (
    <div className="grid gap-2">
      <div className="grid gap-1">
        {featured.map(([k, v]) => (
          <div
            key={k}
            className="grid grid-cols-[auto_1fr] gap-2 border-b border-(--color-border) pb-1 last:border-0"
          >
            <span className={`${labelSize} text-(--color-muted-foreground)`}>{k}</span>
            <span className={`${valueSize} truncate font-mono`} title={v}>
              {v}
            </span>
          </div>
        ))}
      </div>

      {allRows.length > featured.length && (
        <Dialog>
          <DialogTrigger asChild>
            <Button
              variant="outline"
              size="sm"
              className="h-7 w-full justify-center text-xs"
            >
              <Info className="mr-1 h-3.5 w-3.5" />
              View all {allRows.length} EXIF fields
            </Button>
          </DialogTrigger>
          <DialogContent className="max-w-2xl">
            <DialogHeader>
              <DialogTitle>EXIF metadata</DialogTitle>
              <DialogDescription>{allRows.length} fields</DialogDescription>
            </DialogHeader>
            <div className="max-h-[60vh] overflow-y-auto rounded-md border border-(--color-border)">
              <table className="w-full border-collapse text-xs">
                <tbody>
                  {allRows.map(([k, v], i) => (
                    <tr
                      key={k}
                      className={i % 2 === 1 ? 'bg-(--color-muted)' : undefined}
                    >
                      <td className="w-1/3 whitespace-nowrap px-3 py-1.5 align-top font-medium text-(--color-muted-foreground)">
                        {k}
                      </td>
                      <td className="break-all px-3 py-1.5 font-mono">{v}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </DialogContent>
        </Dialog>
      )}
    </div>
  )
}
