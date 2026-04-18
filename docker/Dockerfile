# syntax=docker/dockerfile:1
ARG BUILD_TYPE=Release
ARG JEMALLOC=ON
# Override to use a closer mirror, e.g.:
#   TUNA (Asia):   https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports
#   JAIST (Japan): http://ftp.jaist.ac.jp/pub/Linux/ubuntu-ports
#   AARNET (AU):   https://mirror.aarnet.edu.au/pub/ubuntu/ports
ARG APT_MIRROR=http://ports.ubuntu.com/ubuntu-ports

# ── deps stage: apt + Drogon + AWS SDK ───────────────────────────────────────
# This stage is cached after the first build. It only rebuilds when the FROM
# image, apt packages, or dependency versions change — never on source edits.
FROM ubuntu:24.04 AS deps

ARG APT_MIRROR
RUN if [ "$APT_MIRROR" != "http://ports.ubuntu.com/ubuntu-ports" ]; then \
        sed -i "s|http://ports.ubuntu.com/ubuntu-ports|${APT_MIRROR}|g" \
            /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null || \
        sed -i "s|http://ports.ubuntu.com/ubuntu-ports|${APT_MIRROR}|g" \
            /etc/apt/sources.list; \
    fi

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    build-essential cmake ninja-build pkg-config git curl \
    libvips-dev \
    libjsoncpp-dev uuid-dev libssl-dev zlib1g-dev \
    libbrotli-dev libc-ares-dev \
    libjemalloc-dev

RUN git clone --depth 1 --branch v1.9.6 --recurse-submodules \
        https://github.com/drogonframework/drogon /opt/drogon \
    && cmake -S /opt/drogon -B /opt/drogon/build \
        -GNinja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=17 \
        -DBUILD_CTL=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_DROGON_SHARED=OFF \
        -DBUILD_TESTING=OFF \
        -DUSE_SUBMODULE=ON \
    && cmake --build /opt/drogon/build \
    && cmake --install /opt/drogon/build \
    && rm -rf /opt/drogon

RUN git clone --depth 1 --branch 1.11.356 --recurse-submodules \
        https://github.com/aws/aws-sdk-cpp /opt/aws-sdk \
    && cmake -S /opt/aws-sdk -B /opt/aws-sdk/build \
        -GNinja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_ONLY="s3" \
        -DENABLE_TESTING=OFF \
        -DAUTORUN_UNIT_TESTS=OFF \
        -DBUILD_SHARED_LIBS=OFF \
    && cmake --build /opt/aws-sdk/build \
    && cmake --install /opt/aws-sdk/build \
    && rm -rf /opt/aws-sdk

# ── builder stage: compile imago source ──────────────────────────────────────
# Only this stage reruns on source changes. deps above stays cached.
FROM deps AS builder

ARG BUILD_TYPE
ARG JEMALLOC

WORKDIR /src
COPY . .

RUN if [ "$BUILD_TYPE" = "Debug" ]; then \
        PRESET=debug; \
    elif [ "$JEMALLOC" = "ON" ]; then \
        PRESET=release-jemalloc; \
    else \
        PRESET=release; \
    fi \
    && rm -rf build/release build/debug build/release-jemalloc \
    && cmake --preset $PRESET \
    && cmake --build --preset $PRESET

RUN if [ "$BUILD_TYPE" = "Debug" ]; then \
        cp /src/build/debug/imago /usr/local/bin/imago; \
    elif [ "$JEMALLOC" = "ON" ]; then \
        cp /src/build/release-jemalloc/imago /usr/local/bin/imago; \
    else \
        cp /src/build/release/imago /usr/local/bin/imago; \
    fi

# ── runtime stage ─────────────────────────────────────────────────────────────
FROM ubuntu:24.04

ARG APT_MIRROR
RUN if [ "$APT_MIRROR" != "http://ports.ubuntu.com/ubuntu-ports" ]; then \
        sed -i "s|http://ports.ubuntu.com/ubuntu-ports|${APT_MIRROR}|g" \
            /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null || \
        sed -i "s|http://ports.ubuntu.com/ubuntu-ports|${APT_MIRROR}|g" \
            /etc/apt/sources.list; \
    fi

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    libvips42 \
    libheif-plugin-aomenc libheif-plugin-libde265 \
    libssl3 zlib1g libjsoncpp25 libuuid1 \
    libc-ares2 \
    libjemalloc2 \
    curl

COPY --from=builder /usr/local/bin/imago /usr/local/bin/imago
COPY --from=builder /src/asan /app/asan

ENV IMAGO_PORT=8080
EXPOSE 8080

ENTRYPOINT ["/usr/local/bin/imago"]
