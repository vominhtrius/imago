#!/usr/bin/env bash
# Downloads the DIV2K validation dataset, converts it to 4 formats,
# saves files into benchmark/dataset/, and uploads them to MinIO.
#
# Downloads and converts DIV2K images into benchmark/dataset/.
# MinIO upload is handled separately by the seeder service in docker-compose.yml.
#
# Prerequisites:
#   - vips CLI  (brew install vips / apt install libvips-tools)
#
# Usage:
#   ./seed.sh

set -euo pipefail

# Ensure Linuxbrew tools (vips) are on PATH
export PATH="/home/linuxbrew/.linuxbrew/bin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATASET_DIR="$SCRIPT_DIR/dataset"
TMP_DIR="/tmp/divk2-src"
DATA_URL="http://data.vision.ee.ethz.ch/cvl/DIV2K/DIV2K_valid_HR.zip"

# --- check prerequisites -------------------------------------------------------
command -v vips  >/dev/null 2>&1 || { echo "ERROR: vips CLI not found. Install libvips-tools."; exit 1; }

echo "==> Preparing dataset directory: $DATASET_DIR"
mkdir -p "$DATASET_DIR" "$TMP_DIR"

# --- download DIV2K validation set --------------------------------------------
ZIP="$TMP_DIR/DIV2K_valid_HR.zip"
if [ ! -f "$ZIP" ]; then
  echo "==> Downloading DIV2K validation HR images (~430MB)..."
  curl -L -o "$ZIP" "$DATA_URL"
fi

echo "==> Extracting..."
unzip -q -n "$ZIP" -d "$TMP_DIR"

SRC_DIR="$TMP_DIR/DIV2K_valid_HR"
PNG_FILES=("$SRC_DIR"/*.png)
echo "==> Found ${#PNG_FILES[@]} source PNG images."

# --- convert to 4 formats -----------------------------------------------------
echo "==> Converting images (jpg / webp / avif)..."

LIST_FILE="$DATASET_DIR/list.txt"
: > "$LIST_FILE"

for src in "$SRC_DIR"/*.png; do
  base="$(basename "$src" .png)"

  # JPEG quality 80, progressive
  dst_jpg="$DATASET_DIR/${base}.jpg"
  [ -f "$dst_jpg" ] || vips jpegsave "$src" "$dst_jpg" --Q 80 --optimize-coding --interlace

  # WebP quality 75
  dst_webp="$DATASET_DIR/${base}.webp"
  [ -f "$dst_webp" ] || vips webpsave "$src" "$dst_webp" --Q 75

  # AVIF quality 65, effort 0 (speed)
  dst_avif="$DATASET_DIR/${base}.avif"
  [ -f "$dst_avif" ] || vips heifsave "$src" "$dst_avif" --Q 65 --compression av1 --effort 0

  # PNG (copy as-is — already PNG)
  dst_png="$DATASET_DIR/${base}.png"
  [ -f "$dst_png" ] || cp "$src" "$dst_png"

  echo "${base}.jpg"   >> "$LIST_FILE"
  echo "${base}.webp"  >> "$LIST_FILE"
  echo "${base}.avif"  >> "$LIST_FILE"
  echo "${base}.png"   >> "$LIST_FILE"
done

echo "==> Generated $(wc -l < "$LIST_FILE") entries in list.txt"
echo "==> Conversion done. Run 'make prepare-dataset' to upload to MinIO."
