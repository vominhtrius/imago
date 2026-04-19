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

// Throws when the payload doesn't look like settings JSON — caller is
// expected to surface the message in the UI.
export function parseSettingsJson(raw: string): AppSettings {
  const parsed = JSON.parse(raw) as unknown
  if (!parsed || typeof parsed !== 'object') {
    throw new Error('JSON payload is not an object')
  }
  // Shallow-merge onto defaults so old exports missing new fields still
  // load, and unknown fields are silently dropped.
  const merged: AppSettings = { ...DEFAULT_SETTINGS }
  for (const k of Object.keys(DEFAULT_SETTINGS) as Array<keyof AppSettings>) {
    const v = (parsed as Record<string, unknown>)[k]
    if (v === undefined) continue
    const expected = typeof DEFAULT_SETTINGS[k]
    if (typeof v !== expected) {
      throw new Error(`field "${k}" should be ${expected}, got ${typeof v}`)
    }
    ;(merged as unknown as Record<string, unknown>)[k] = v
  }
  return merged
}

export function settingsToJson(s: AppSettings): string {
  return JSON.stringify(s, null, 2)
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
