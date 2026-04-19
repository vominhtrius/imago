import * as React from 'react'

export interface AppSettings {
  imagoBaseUrl: string
  imgproxyBaseUrl: string
  s3Endpoint: string
  s3Region: string
  s3AccessKeyId: string
  s3SecretAccessKey: string
  s3Bucket: string
  s3ForcePathStyle: boolean
  s3KeyPrefix: string
}

const STORAGE_KEY = 'imago-playground.settings.v1'

// Defaults wired for the docker-compose `web` profile stack:
//   nginx reverse-proxies /api/imago → imago:8080 and /api/imgproxy → imgproxy:8080.
//   Browser talks directly to MinIO on the host port with the public dev creds
//   baked into docker/docker-compose.yml.
export const DEFAULT_SETTINGS: AppSettings = {
  imagoBaseUrl: '/api/imago',
  imgproxyBaseUrl: '/api/imgproxy',
  s3Endpoint: 'http://localhost:9000',
  s3Region: 'us-east-1',
  s3AccessKeyId: 'minioadmin',
  s3SecretAccessKey: 'minioadmin',
  s3Bucket: '',
  s3ForcePathStyle: true,
  s3KeyPrefix: 'uploads/',
}

export function loadSettings(): AppSettings {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    if (!raw) return DEFAULT_SETTINGS
    const parsed = JSON.parse(raw) as Partial<AppSettings>
    return { ...DEFAULT_SETTINGS, ...parsed }
  } catch {
    return DEFAULT_SETTINGS
  }
}

export function saveSettings(s: AppSettings) {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(s))
}

interface SettingsContextValue {
  settings: AppSettings
  update: (patch: Partial<AppSettings>) => void
  replace: (next: AppSettings) => void
}

const SettingsContext = React.createContext<SettingsContextValue | null>(null)

export function SettingsProvider({ children }: { children: React.ReactNode }) {
  const [settings, setSettings] = React.useState<AppSettings>(() => loadSettings())

  const update = React.useCallback((patch: Partial<AppSettings>) => {
    setSettings((prev) => {
      const next = { ...prev, ...patch }
      saveSettings(next)
      return next
    })
  }, [])

  const replace = React.useCallback((next: AppSettings) => {
    saveSettings(next)
    setSettings(next)
  }, [])

  const value = React.useMemo(() => ({ settings, update, replace }), [settings, update, replace])
  return React.createElement(SettingsContext.Provider, { value }, children)
}

export function useSettings() {
  const ctx = React.useContext(SettingsContext)
  if (!ctx) throw new Error('useSettings must be used inside SettingsProvider')
  return ctx
}

export function s3IsConfigured(s: AppSettings) {
  return Boolean(s.s3AccessKeyId && s.s3SecretAccessKey && s.s3Bucket && s.s3Region)
}
