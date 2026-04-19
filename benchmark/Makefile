# Benchmark: imgproxy vs imago
#
# Quick start:
#   make prepare-dataset      # download DIV2K, convert, upload to MinIO
#   make bench-imgproxy       # 5-minute jpeg resize benchmark against imgproxy
#   make bench-imago          # same against imago
#
# Variables:
#   FORMAT   = jpeg | webp | avif | png       (default: jpeg)
#   METHOD   = resize | crop | convert        (default: resize)
#   WIDTH    = output width  (default: 512)
#   HEIGHT   = output height (default: 512)
#   DURATION = k6 duration   (default: 30s for bench-quick, override with DURATION=5m)
#   WORKERS  = thread/worker count for imago (default: nproc)
#   BUCKET   = MinIO bucket name (default: divk2)

DURATION ?= 30s
FORMAT   ?= jpeg
METHOD   ?= resize
WIDTH    ?= 512
HEIGHT   ?= 512
BUCKET   ?= divk2
NCPU     := $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu)
WORKERS  ?= $(NCPU)
VUS      ?= $(NCPU)

DC          := docker compose
RESULTS_DIR := results
GIT_VERSION := $(shell git -C .. rev-parse --short HEAD 2>/dev/null)
RUN_TS      ?= $(shell date +%Y%m%d_%H%M%S)
VERSION     ?= $(if $(GIT_VERSION),$(GIT_VERSION),dev)_$(RUN_TS)
export RUN_TS VERSION

.PHONY: prepare-dataset build-imago \
        bench-imgproxy bench-imago \
        bench-all-imgproxy bench-all-imago \
        bench-quick bench-all chart chart-memory \
        down clean _up-imgproxy _up-imago _k6 _down

# ── Quick smoke test (30s each) + full suite ─────────────────────────────────
#   make bench-quick                  → 30s resize jpeg only, then chart
#   make bench-quick DURATION=1m      → 1m  resize jpeg only, then chart
#   make bench-all                    → 30s all methods × all formats, then chart
#   make bench-all   DURATION=5m      → 5m  full suite, then chart

bench-quick: _check-dataset _up-imgproxy
	$(MAKE) _k6 TOOL=imgproxy METHOD=resize FORMAT=jpeg
	$(MAKE) _down
	$(MAKE) _up-imago
	$(MAKE) _k6 TOOL=imago   METHOD=resize FORMAT=jpeg
	$(MAKE) _down chart chart-memory

bench-all: _check-dataset _up-imgproxy
	$(MAKE) _k6 TOOL=imgproxy METHOD=resize  FORMAT=jpeg
	$(MAKE) _k6 TOOL=imgproxy METHOD=resize  FORMAT=webp
	$(MAKE) _k6 TOOL=imgproxy METHOD=crop    FORMAT=jpeg
	$(MAKE) _k6 TOOL=imgproxy METHOD=convert FORMAT=webp
	$(MAKE) _down
	$(MAKE) _up-imago
	$(MAKE) _k6 TOOL=imago   METHOD=resize  FORMAT=jpeg
	$(MAKE) _k6 TOOL=imago   METHOD=resize  FORMAT=webp
	$(MAKE) _k6 TOOL=imago   METHOD=crop    FORMAT=jpeg
	$(MAKE) _k6 TOOL=imago   METHOD=convert FORMAT=webp
	$(MAKE) _down chart chart-memory

chart:
	pip3 install -q pandas matplotlib --break-system-packages 2>/dev/null || pip3 install -q pandas matplotlib
	python3 chart.py --csv $(RESULTS_DIR)/summary.csv --out $(RESULTS_DIR)/charts \
		$(if $(VERSION),--version $(VERSION),)
	@echo "==> Charts written to $(RESULTS_DIR)/charts/"

chart-memory:
	pip3 install -q pandas matplotlib --break-system-packages 2>/dev/null || pip3 install -q pandas matplotlib
	python3 chart_memory.py --results $(RESULTS_DIR) --out $(RESULTS_DIR)/charts
	@echo "==> Memory charts written to $(RESULTS_DIR)/charts/"

# ── Build ────────────────────────────────────────────────────────────────────

build-imago:
	WORKERS=$(WORKERS) $(DC) build imago

# ── Dataset ──────────────────────────────────────────────────────────────────

prepare-dataset:
	@echo "==> Converting images locally..."
	./seed.sh
	@echo "==> Starting MinIO for seeding..."
	$(DC) up -d minio
	@echo "==> Uploading dataset to MinIO..."
	BUCKET=$(BUCKET) $(DC) --profile seed run --rm seeder
	$(DC) stop minio

# ── Benchmarks ───────────────────────────────────────────────────────────────

bench-imgproxy: _check-dataset _up-imgproxy
	$(MAKE) _k6 TOOL=imgproxy
	$(MAKE) _down

bench-imago: _check-dataset _up-imago
	$(MAKE) _k6 TOOL=imago
	$(MAKE) _down

# Run all 3 methods back-to-back for one service
bench-all-imgproxy: _check-dataset _up-imgproxy
	$(MAKE) _k6 TOOL=imgproxy METHOD=resize
	$(MAKE) _k6 TOOL=imgproxy METHOD=crop
	$(MAKE) _k6 TOOL=imgproxy METHOD=convert
	$(MAKE) _down

bench-all-imago: _check-dataset _up-imago
	$(MAKE) _k6 TOOL=imago METHOD=resize
	$(MAKE) _k6 TOOL=imago METHOD=crop
	$(MAKE) _k6 TOOL=imago METHOD=convert
	$(MAKE) _down

# ── Internal targets ──────────────────────────────────────────────────────────

_up-imgproxy:
	$(DC) up -d --wait minio imgproxy

_up-imago:
	WORKERS=$(WORKERS) $(DC) up -d --wait minio imago

_k6:
	@mkdir -p $(RESULTS_DIR)
	$(eval TS    := $(shell date +%Y%m%d_%H%M%S))
	$(eval OUT   := $(RESULTS_DIR)/$(TOOL)_$(METHOD)_$(FORMAT)_$(WIDTH)x$(HEIGHT)_$(TS).txt)
	$(eval STATS := $(RESULTS_DIR)/$(TOOL)_$(METHOD)_$(FORMAT)_$(WIDTH)x$(HEIGHT)_$(TS).stats)
	@echo "==> k6: TOOL=$(TOOL) METHOD=$(METHOD) FORMAT=$(FORMAT) $(WIDTH)x$(HEIGHT) vus=$(VUS) duration=$(DURATION)"
	@CID=$$($(DC) ps -q $(TOOL)); \
	( while true; do \
	    printf "%s," "$$(date +%s)"; \
	    docker stats --no-stream --format "{{.CPUPerc}},{{.MemUsage}}" $$CID 2>/dev/null; \
	    sleep 1; \
	  done ) > $(STATS) & \
	SPID=$$!; \
	$(DC) --profile bench run --rm \
	    -e TOOL=$(TOOL) \
	    -e METHOD=$(METHOD) \
	    -e FORMAT=$(FORMAT) \
	    -e WIDTH=$(WIDTH) \
	    -e HEIGHT=$(HEIGHT) \
	    -e BUCKET=$(BUCKET) \
	    k6 run -u $(VUS) -d $(DURATION) /benchmark/k6.js \
	    | tee $(OUT); \
	kill $$SPID 2>/dev/null || true
	@echo "==> Results saved to $(OUT)"
	python3 parse_summary.py $(OUT) $(TOOL) $(METHOD) $(FORMAT) $(WIDTH) $(HEIGHT) $(VUS) $(VERSION) $(STATS)

_check-dataset:
	@test -f dataset/list.txt || \
		(echo "ERROR: dataset/list.txt missing. Run 'make prepare-dataset' first." && exit 1)

_down:
	@if $(DC) ps -q imago 2>/dev/null | grep -q .; then \
	    echo "==> Stopping imago to flush jemalloc stats"; \
	    mkdir -p $(RESULTS_DIR); \
	    LOG=$(RESULTS_DIR)/imago_$$(date +%Y%m%d_%H%M%S).log; \
	    $(DC) stop -t 15 imago >/dev/null 2>&1 || true; \
	    $(DC) logs --no-color --no-log-prefix imago > $$LOG 2>&1 || true; \
	    echo "==> imago log (incl. jemalloc dump) saved to $$LOG"; \
	fi
	$(DC) down

# ── Cleanup ───────────────────────────────────────────────────────────────────

down:
	$(DC) down

clean:
	$(DC) down -v
	rm -rf dataset/
