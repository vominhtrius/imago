import { Input } from '@/components/ui/input'
import { cn } from '@/lib/utils'

interface NumberFieldProps {
  value: number | ''
  onChange: (v: number | '') => void
  min?: number
  max?: number
  placeholder?: string
  className?: string
}

export function NumberField({ value, onChange, min, max, placeholder, className }: NumberFieldProps) {
  return (
    <Input
      type="number"
      inputMode="numeric"
      min={min}
      max={max}
      value={value === '' ? '' : String(value)}
      placeholder={placeholder}
      onChange={(e) => {
        const raw = e.target.value
        if (raw === '') {
          onChange('')
          return
        }
        const n = Number(raw)
        if (Number.isFinite(n)) onChange(n)
      }}
      className={cn(className)}
    />
  )
}
