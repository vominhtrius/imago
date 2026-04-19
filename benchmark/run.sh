#!/usr/bin/env bash
# Benchmark script for imago vs imgproxy
# Requires: wrk (https://github.com/wg/wrk)
# Run one service at a time to avoid resource contention.
#
# Usage:
#   TARGET=http://localhost:8080 BUCKET=images ./benchmark/run.sh   # imago
#   TARGET=http://localhost:8082 BUCKET=images ./benchmark/run.sh   # imgproxy

set -euo pipefail

TARGET="${TARGET:-http://localhost:8080}"
BUCKET="${BUCKET:-images}"
WRK="${WRK:-wrk}"
DURATION="${DURATION:-30s}"

RESULTS_DIR="$(dirname "$0")/results"
mkdir -p "$RESULTS_DIR"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
RESULT_FILE="$RESULTS_DIR/wrk_${TIMESTAMP}.txt"

# Tee all output to timestamped file alongside terminal
exec > >(tee "$RESULT_FILE") 2>&1
echo "==> Results will be saved to: $RESULT_FILE"

IMG_4MP="test/4mp.jpg"
IMG_10MP="test/10mp.jpg"

# ── imago URL builders ───────────────────────────────────────────────────────
resize_url() {
    local key="$1" w="$2" h="$3" fit="${4:-fit}" output="${5:-webp}"
    echo "${TARGET}/resize/${BUCKET}/${key}?w=${w}&h=${h}&fit=${fit}&output=${output}"
}

crop_url() {
    local key="$1" w="$2" h="$3" gravity="${4:-attention}" output="${5:-webp}"
    echo "${TARGET}/crop/${BUCKET}/${key}?w=${w}&h=${h}&gravity=${gravity}&output=${output}"
}

convert_url() {
    local key="$1" output="${2:-webp}"
    echo "${TARGET}/convert/${BUCKET}/${key}?output=${output}"
}

# ── imgproxy URL builder (plain S3 source, no signing) ──────────────────────
imgproxy_url() {
    local key="$1" w="$2" h="$3" fit="${4:-fit}" output="${5:-webp}"
    local encoded_url
    encoded_url=$(python3 -c "import urllib.parse; print(urllib.parse.quote('s3://${BUCKET}/${key}', safe=''))")
    echo "${TARGET}/insecure/resize:${fit}:${w}:${h}:0/plain/${encoded_url}@${output}"
}

run_wrk() {
    local label="$1" url="$2" conc="$3"
    echo ""
    echo "=== ${label} | c=${conc} | ${DURATION} ==="
    echo "URL: ${url}"
    "$WRK" -t2 -c"${conc}" -d"${DURATION}" --latency "$url"
}

echo "Target: ${TARGET}"
echo "Bucket: ${BUCKET}"
echo "Duration per test: ${DURATION}"
echo "========================================"

# B1 — resize 800×600 fit, 4MP JPEG → WebP
for c in 10 50 100; do
    run_wrk "B1-resize-fit-4MP" "$(resize_url "$IMG_4MP" 800 600 fit webp)" "$c"
done

# B2 — resize 800×600 fill, 4MP JPEG → WebP
for c in 10 50 100; do
    run_wrk "B2-resize-fill-4MP" "$(resize_url "$IMG_4MP" 800 600 fill webp)" "$c"
done

# B3 — crop 800×600 attention, 4MP JPEG → WebP
for c in 10 50 100; do
    run_wrk "B3-crop-4MP" "$(crop_url "$IMG_4MP" 800 600 attention webp)" "$c"
done

# B4 — convert only, 4MP JPEG → WebP
for c in 10 50 100; do
    run_wrk "B4-convert-4MP" "$(convert_url "$IMG_4MP" webp)" "$c"
done

# B5 — resize 1920×1080 fill, 10MP JPEG → WebP
run_wrk "B5-resize-fill-10MP" "$(resize_url "$IMG_10MP" 1920 1080 fill webp)" 50

echo ""
echo "========================================"
echo "Done."
