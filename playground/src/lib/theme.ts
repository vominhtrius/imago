import * as React from 'react'

export type Theme = 'light' | 'dark' | 'system'
export type ResolvedTheme = 'light' | 'dark'

const STORAGE_KEY = 'imago-playground.theme.v1'

function readStored(): Theme {
  try {
    const v = localStorage.getItem(STORAGE_KEY)
    if (v === 'light' || v === 'dark' || v === 'system') return v
  } catch {}
  return 'system'
}

function systemPrefersDark() {
  return window.matchMedia('(prefers-color-scheme: dark)').matches
}

function resolve(theme: Theme): ResolvedTheme {
  if (theme === 'system') return systemPrefersDark() ? 'dark' : 'light'
  return theme
}

function apply(resolved: ResolvedTheme) {
  const root = document.documentElement
  root.classList.toggle('dark', resolved === 'dark')
}

interface ThemeContextValue {
  theme: Theme
  resolved: ResolvedTheme
  setTheme: (t: Theme) => void
}

const ThemeContext = React.createContext<ThemeContextValue | null>(null)

export function ThemeProvider({ children }: { children: React.ReactNode }) {
  const [theme, setThemeState] = React.useState<Theme>(() => readStored())
  const [resolved, setResolved] = React.useState<ResolvedTheme>(() => resolve(readStored()))

  React.useEffect(() => {
    const r = resolve(theme)
    setResolved(r)
    apply(r)
    try { localStorage.setItem(STORAGE_KEY, theme) } catch {}
  }, [theme])

  // Follow the OS when the user picks "system" and changes their preference.
  React.useEffect(() => {
    if (theme !== 'system') return
    const mq = window.matchMedia('(prefers-color-scheme: dark)')
    const onChange = () => {
      const r: ResolvedTheme = mq.matches ? 'dark' : 'light'
      setResolved(r)
      apply(r)
    }
    mq.addEventListener('change', onChange)
    return () => mq.removeEventListener('change', onChange)
  }, [theme])

  const value = React.useMemo<ThemeContextValue>(
    () => ({ theme, resolved, setTheme: setThemeState }),
    [theme, resolved],
  )
  return React.createElement(ThemeContext.Provider, { value }, children)
}

export function useTheme() {
  const ctx = React.useContext(ThemeContext)
  if (!ctx) throw new Error('useTheme must be used inside ThemeProvider')
  return ctx
}
