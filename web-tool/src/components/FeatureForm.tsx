import { useState } from 'react'
import { Play, Loader2 } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Label } from '@/components/ui/label'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { NumberField } from '@/components/ui/slider-number'
import { SourcePicker, type SourceState } from '@/components/SourcePicker'
import { InputPreview } from '@/components/InputPreview'
import { ResultPanel, type ServiceOutcome } from '@/components/ResultPanel'
import {
  buildImagoUrl,
  buildImgproxyUrl,
  fetchImage,
  type FetchResult,
  type Operation,
} from '@/lib/imago'
import { useSettings } from '@/lib/settings'

const DEFAULT = '__default__'
const OUTPUTS = ['webp', 'jpeg', 'png', 'avif']
const FITS = ['fit', 'fill', 'fill-down', 'force']
// Tokens match imgproxy. imago accepts the same set (plus "entropy" as an
// imago-only extra); imgproxy will error on "entropy".
const GRAVITIES: Array<{ value: string; label: string }> = [
  { value: 'ce', label: 'ce — center' },
  { value: 'no', label: 'no — north' },
  { value: 'so', label: 'so — south' },
  { value: 'ea', label: 'ea — east' },
  { value: 'we', label: 'we — west' },
  { value: 'noea', label: 'noea — north-east' },
  { value: 'nowe', label: 'nowe — north-west' },
  { value: 'soea', label: 'soea — south-east' },
  { value: 'sowe', label: 'sowe — south-west' },
  { value: 'sm', label: 'sm — smart' },
  { value: 'entropy', label: 'entropy (imago only)' },
]

interface ParamsState {
  w: number | ''
  h: number | ''
  fit: string
  gravity: string
  output: string
  quality: number | ''
}

const DEFAULT_PARAMS: ParamsState = {
  w: '',
  h: '',
  fit: DEFAULT,
  gravity: DEFAULT,
  output: DEFAULT,
  quality: '',
}

interface Props {
  operation: Operation
  title: string
  description: string
  showResize: boolean
  showCrop: boolean
}

interface ServiceState {
  result: FetchResult | null
  error: string | null
  url: string
}

const EMPTY_SERVICE: ServiceState = { result: null, error: null, url: '' }

export function FeatureForm({ operation, title, description, showResize, showCrop }: Props) {
  const { settings } = useSettings()
  const [source, setSource] = useState<SourceState>({
    bucket: settings.s3Bucket,
    key: '',
    pickedFile: null,
  })
  const [params, setParams] = useState<ParamsState>(DEFAULT_PARAMS)
  const [imago, setImago] = useState<ServiceState>(EMPTY_SERVICE)
  const [imgproxy, setImgproxy] = useState<ServiceState>(EMPTY_SERVICE)
  const [loading, setLoading] = useState(false)

  const resolvedBucket = source.bucket || settings.s3Bucket

  const wNum = params.w === '' ? 0 : Number(params.w)
  const hNum = params.h === '' ? 0 : Number(params.h)
  // Resize needs at least one dimension; crop needs both (otherwise the crop
  // window is undefined). Convert ignores w/h entirely.
  const dimsOk =
    operation === 'convert'
      ? true
      : operation === 'crop'
        ? wNum > 0 && hNum > 0
        : wNum > 0 || hNum > 0

  const missingReason = !resolvedBucket || !source.key
    ? 'Provide a bucket and object key (or upload a file).'
    : !dimsOk
      ? operation === 'crop'
        ? 'Both width and height are required for crop.'
        : 'Provide at least one of width or height (use Convert to only transcode).'
      : ''

  const canSubmit = Boolean(resolvedBucket && source.key && dimsOk && !loading)

  const paramPayload = (): Record<string, unknown> => {
    const p: Record<string, unknown> = {}
    if (params.output !== DEFAULT) p.output = params.output
    if (params.quality !== '') p.quality = params.quality
    if (showResize || showCrop) {
      if (params.w !== '') p.w = params.w
      if (params.h !== '') p.h = params.h
    }
    if (showResize && params.fit !== DEFAULT) p.fit = params.fit
    if (showCrop && params.gravity !== DEFAULT) p.gravity = params.gravity
    return p
  }

  const payload = paramPayload()
  const imagoPreviewUrl =
    resolvedBucket && source.key
      ? buildImagoUrl(settings, operation, resolvedBucket, source.key, payload)
      : ''
  const imgproxyPreviewUrl =
    resolvedBucket && source.key && settings.imgproxyBaseUrl
      ? buildImgproxyUrl(settings, operation, resolvedBucket, source.key, payload)
      : ''

  const onSubmit = async () => {
    if (!canSubmit) return
    setLoading(true)
    setImago(EMPTY_SERVICE)
    setImgproxy(EMPTY_SERVICE)

    const imagoUrl = buildImagoUrl(settings, operation, resolvedBucket, source.key, payload)
    const imgproxyUrl = settings.imgproxyBaseUrl
      ? buildImgproxyUrl(settings, operation, resolvedBucket, source.key, payload)
      : ''

    setImago((s) => ({ ...s, url: imagoUrl }))
    if (imgproxyUrl) setImgproxy((s) => ({ ...s, url: imgproxyUrl }))

    const jobs: Array<Promise<unknown>> = []

    jobs.push(
      fetchImage(imagoUrl, { label: 'imago' })
        .then((r) => setImago({ url: imagoUrl, result: r, error: null }))
        .catch((e: unknown) =>
          setImago({
            url: imagoUrl,
            result: null,
            error: e instanceof Error ? e.message : String(e),
          }),
        ),
    )

    if (imgproxyUrl) {
      jobs.push(
        // imgproxy emits Server-Timing as `imgproxy;dur=…`; fall back to
        // `total` which some builds emit.
        fetchImage(imgproxyUrl, { label: 'imgproxy', timingNames: ['imgproxy', 'total'] })
          .then((r) => setImgproxy({ url: imgproxyUrl, result: r, error: null }))
          .catch((e: unknown) =>
            setImgproxy({
              url: imgproxyUrl,
              result: null,
              error: e instanceof Error ? e.message : String(e),
            }),
          ),
      )
    }

    await Promise.all(jobs)
    setLoading(false)
  }

  const outcomes: ServiceOutcome[] = [
    { label: 'imago', requestUrl: imago.url, result: imago.result, error: imago.error },
  ]
  if (settings.imgproxyBaseUrl) {
    outcomes.push({
      label: 'imgproxy',
      requestUrl: imgproxy.url,
      result: imgproxy.result,
      error: imgproxy.error,
    })
  }

  return (
    <div className="grid gap-4 lg:grid-cols-[minmax(0,420px)_minmax(0,1fr)]">
      <Card>
        <CardHeader>
          <CardTitle>{title}</CardTitle>
          <p className="text-sm text-(--color-muted-foreground)">{description}</p>
        </CardHeader>
        <CardContent className="grid gap-4">
          <SourcePicker value={source} onChange={setSource} />

          <InputPreview source={source} />

          <div className="grid gap-3">
            <h4 className="text-sm font-semibold">Parameters</h4>

            {(showResize || showCrop) && (
              <div className="grid grid-cols-2 gap-3">
                <div className="grid gap-1">
                  <Label>Width</Label>
                  <NumberField
                    value={params.w}
                    onChange={(v) => setParams((p) => ({ ...p, w: v }))}
                    min={0}
                    placeholder={showCrop ? 'required' : '0 = auto'}
                  />
                </div>
                <div className="grid gap-1">
                  <Label>Height</Label>
                  <NumberField
                    value={params.h}
                    onChange={(v) => setParams((p) => ({ ...p, h: v }))}
                    min={0}
                    placeholder={showCrop ? 'required' : '0 = auto'}
                  />
                </div>
              </div>
            )}

            {showResize && (
              <div className="grid gap-1">
                <Label>Fit</Label>
                <Select
                  value={params.fit}
                  onValueChange={(v) => setParams((p) => ({ ...p, fit: v }))}
                >
                  <SelectTrigger>
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value={DEFAULT}>default</SelectItem>
                    {FITS.map((f) => (
                      <SelectItem key={f} value={f}>
                        {f}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>
            )}

            {showCrop && (
              <div className="grid gap-1">
                <Label>Gravity</Label>
                <Select
                  value={params.gravity}
                  onValueChange={(v) => setParams((p) => ({ ...p, gravity: v }))}
                >
                  <SelectTrigger>
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value={DEFAULT}>default</SelectItem>
                    {GRAVITIES.map((g) => (
                      <SelectItem key={g.value} value={g.value}>
                        {g.label}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>
            )}

            <div className="grid grid-cols-2 gap-3">
              <div className="grid gap-1">
                <Label>Output</Label>
                <Select
                  value={params.output}
                  onValueChange={(v) => setParams((p) => ({ ...p, output: v }))}
                >
                  <SelectTrigger>
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value={DEFAULT}>same as source</SelectItem>
                    {OUTPUTS.map((o) => (
                      <SelectItem key={o} value={o}>
                        {o}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>
              <div className="grid gap-1">
                <Label>Quality (1-100)</Label>
                <NumberField
                  value={params.quality}
                  onChange={(v) => setParams((p) => ({ ...p, quality: v }))}
                  min={1}
                  max={100}
                  placeholder="default"
                />
              </div>
            </div>
          </div>

          {(imagoPreviewUrl || imgproxyPreviewUrl) && (
            <div className="grid gap-1">
              {imagoPreviewUrl && (
                <div className="rounded-md bg-(--color-muted) p-2 font-mono text-[11px] break-all">
                  <span className="mr-2 font-semibold text-(--color-primary)">imago</span>
                  {imagoPreviewUrl}
                </div>
              )}
              {imgproxyPreviewUrl && (
                <div className="rounded-md bg-(--color-muted) p-2 font-mono text-[11px] break-all">
                  <span className="mr-2 font-semibold text-(--color-primary)">imgproxy</span>
                  {imgproxyPreviewUrl}
                </div>
              )}
            </div>
          )}

          <Button onClick={onSubmit} disabled={!canSubmit} className="w-full">
            {loading ? <Loader2 className="h-4 w-4 animate-spin" /> : <Play className="h-4 w-4" />}
            Run {operation}
          </Button>
          {!canSubmit && !loading && missingReason && (
            <p className="text-xs text-(--color-muted-foreground)">{missingReason}</p>
          )}
        </CardContent>
      </Card>

      <ResultPanel outcomes={outcomes} loading={loading} />
    </div>
  )
}

export function ResizeForm() {
  return (
    <FeatureForm
      operation="resize"
      title="Resize"
      description="GET /resize/{bucket}/{key} — width/height with fit strategy, optional transcode."
      showResize
      showCrop={false}
    />
  )
}

export function CropForm() {
  return (
    <FeatureForm
      operation="crop"
      title="Crop"
      description="GET /crop/{bucket}/{key} — smart crop to target size with gravity hint."
      showResize={false}
      showCrop
    />
  )
}

export function ConvertForm() {
  return (
    <FeatureForm
      operation="convert"
      title="Convert"
      description="GET /convert/{bucket}/{key} — transcode to webp / jpeg / png / avif."
      showResize={false}
      showCrop={false}
    />
  )
}
