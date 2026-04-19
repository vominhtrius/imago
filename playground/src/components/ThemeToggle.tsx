import { Moon, Sun, Monitor } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { useTheme, type Theme } from '@/lib/theme'

const order: Theme[] = ['light', 'dark', 'system']

const icon: Record<Theme, typeof Sun> = {
  light: Sun,
  dark: Moon,
  system: Monitor,
}

const label: Record<Theme, string> = {
  light: 'Light',
  dark: 'Dark',
  system: 'System',
}

export function ThemeToggle() {
  const { theme, setTheme } = useTheme()
  const Icon = icon[theme]
  const next = order[(order.indexOf(theme) + 1) % order.length]

  return (
    <Button
      variant="outline"
      size="sm"
      onClick={() => setTheme(next)}
      title={`Theme: ${label[theme]} (click for ${label[next]})`}
      aria-label={`Switch to ${label[next]} theme`}
    >
      <Icon className="h-4 w-4" />
      <span className="hidden sm:inline">{label[theme]}</span>
    </Button>
  )
}
