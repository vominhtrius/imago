# imago — Solution Proposal Slides

Slidev presentation based on `docs/solution-proposal-image-service-v2.md`.

## Quick start

```bash
cd docs/slides
npm install
npm run dev        # opens http://localhost:3030
```

## Presenter mode

Once the dev server is running, press **`p`** to open presenter view, or visit
<http://localhost:3030/presenter>. Presenter notes for each slide live in
`<!-- ... -->` comments at the bottom of each slide block in `slides.md`.

## Keyboard shortcuts

- `f` — fullscreen
- `o` — overview
- `d` — dark/light toggle
- `g` — go to slide
- `←` / `→` / `Space` — navigate

## Export

```bash
npm run export-pdf     # slides.pdf with click-step reveals + TOC
npm run export-notes   # notes.pdf (presenter notes only)
npm run build          # static site in ./dist
```

PDF export requires `playwright-chromium` (already in `dependencies`). On first
export Playwright will download its Chromium bundle; ~170 MB.

## Chart images

Slides embed benchmark charts via relative paths:

```
../../benchmark/results/charts/*.png
```

Regenerating the benchmark (`make bench` in `benchmark/`) produces a new run id.
If you want the slides to track a different run, search-and-replace
`9f04418_20260419_002902` in `slides.md`.

## Structure

| Section | Slides |
|---|---|
| Problem & goal | 1–3 |
| Features & architecture | 4–7 |
| Options (market) | 8–11 |
| Benchmark (setup + charts) | 12–18 |
| Recommendation & decision | 19–22 |
| Rollout & risks | 23–24 |
| Ask | 25 |
