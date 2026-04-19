import * as React from 'react'

export type Theme = 'light' | 'dark'

const STORAGE_KEY = 'imago-playground.theme.v1'

function systemPrefersDark() {
  return window.matchMedia('(prefers-color-scheme: dark)').matches
}

function readInitial(): Theme {
  try {
    const v = localStorage.getItem(STORAGE_KEY)
    if (v === 'light' || v === 'dark') return v
  } catch {}
  return systemPrefersDark() ? 'dark' : 'light'
}

function apply(theme: Theme) {
  document.documentElement.classList.toggle('dark', theme === 'dark')
}

interface ThemeContextValue {
  theme: Theme
  setTheme: (t: Theme) => void
  toggle: () => void
}

const ThemeContext = React.createContext<ThemeContextValue | null>(null)

export function ThemeProvider({ children }: { children: React.ReactNode }) {
  const [theme, setThemeState] = React.useState<Theme>(readInitial)

  React.useEffect(() => {
    apply(theme)
    try { localStorage.setItem(STORAGE_KEY, theme) } catch {}
  }, [theme])

  const value = React.useMemo<ThemeContextValue>(
    () => ({
      theme,
      setTheme: setThemeState,
      toggle: () => setThemeState((t) => (t === 'dark' ? 'light' : 'dark')),
    }),
    [theme],
  )
  return React.createElement(ThemeContext.Provider, { value }, children)
}

export function useTheme() {
  const ctx = React.useContext(ThemeContext)
  if (!ctx) throw new Error('useTheme must be used inside ThemeProvider')
  return ctx
}
