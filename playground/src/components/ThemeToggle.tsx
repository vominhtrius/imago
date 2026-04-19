import { Moon, Sun } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { useTheme } from '@/lib/theme'

export function ThemeToggle() {
  const { theme, toggle } = useTheme()
  const isDark = theme === 'dark'
  const Icon = isDark ? Sun : Moon
  const nextLabel = isDark ? 'Light' : 'Dark'

  return (
    <Button
      variant="outline"
      size="sm"
      onClick={toggle}
      title={`Switch to ${nextLabel} theme`}
      aria-label={`Switch to ${nextLabel} theme`}
    >
      <Icon className="h-4 w-4" />
      <span className="hidden sm:inline">{isDark ? 'Dark' : 'Light'}</span>
    </Button>
  )
}
