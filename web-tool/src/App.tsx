import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { SettingsDialog } from '@/components/SettingsDialog'
import { ResizeForm, CropForm, ConvertForm } from '@/components/FeatureForm'
import { SettingsProvider, s3IsConfigured, useSettings } from '@/lib/settings'
import { ImageIcon } from 'lucide-react'

function Header() {
  const { settings } = useSettings()
  return (
    <header className="border-b border-(--color-border) bg-(--color-card)">
      <div className="mx-auto flex max-w-6xl items-center justify-between px-6 py-3">
        <div className="flex items-center gap-2">
          <ImageIcon className="h-5 w-5" />
          <h1 className="text-lg font-semibold">imago web-tool</h1>
          <span className="rounded bg-(--color-muted) px-1.5 py-0.5 text-xs text-(--color-muted-foreground)">
            imago → {settings.imagoBaseUrl}
          </span>
          {settings.imgproxyBaseUrl && (
            <span className="rounded bg-(--color-muted) px-1.5 py-0.5 text-xs text-(--color-muted-foreground)">
              imgproxy → {settings.imgproxyBaseUrl}
            </span>
          )}
          {!s3IsConfigured(settings) && (
            <span className="rounded bg-(--color-destructive)/10 px-1.5 py-0.5 text-xs text-(--color-destructive)">
              S3 not configured
            </span>
          )}
        </div>
        <SettingsDialog />
      </div>
    </header>
  )
}

function Shell() {
  return (
    <div className="min-h-full flex flex-col">
      <Header />
      <main className="mx-auto w-full max-w-6xl flex-1 px-6 py-6">
        <Tabs defaultValue="resize">
          <TabsList>
            <TabsTrigger value="resize">Resize</TabsTrigger>
            <TabsTrigger value="crop">Crop</TabsTrigger>
            <TabsTrigger value="convert">Convert</TabsTrigger>
          </TabsList>
          <TabsContent value="resize">
            <ResizeForm />
          </TabsContent>
          <TabsContent value="crop">
            <CropForm />
          </TabsContent>
          <TabsContent value="convert">
            <ConvertForm />
          </TabsContent>
        </Tabs>
      </main>
      <footer className="border-t border-(--color-border) px-6 py-3 text-center text-xs text-(--color-muted-foreground)">
        Dev only · S3 credentials are stored in localStorage and sent directly to AWS from your
        browser.
      </footer>
    </div>
  )
}

export default function App() {
  return (
    <SettingsProvider>
      <Shell />
    </SettingsProvider>
  )
}
