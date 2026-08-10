# syntax=docker/dockerfile:1.7@sha256:a57df69d0ea827fb7266491f2813635de6f17269be881f696fbfdf2d83dda33e

ARG DEBIAN_IMAGE=debian:bookworm-slim@sha256:7b140f374b289a7c2befc338f42ebe6441b7ea838a042bbd5acbfca6ec875818

FROM ${DEBIAN_IMAGE} AS build

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        libcurl4-openssl-dev \
        libpq-dev \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY resources ./resources
COPY src ./src

RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DPCT_BUILD_BENCHMARKS=OFF \
        -DPCT_WARNINGS_AS_ERRORS=ON \
    && cmake --build /build --target personal-chess-tutor

FROM ${DEBIAN_IMAGE} AS runtime

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        curl \
        libcurl4 \
        libpq5 \
        stockfish \
    && mkdir -p /licenses /opt/plywise/resources /var/lib/plywise \
    && cp /usr/share/common-licenses/GPL-3 /licenses/stockfish-GPL-3.0.txt \
    && cp /usr/share/doc/stockfish/copyright /licenses/stockfish-debian-copyright.txt \
    && groupadd --gid 10001 plywise \
    && useradd --uid 10001 --gid 10001 --no-create-home --home-dir /var/lib/plywise plywise \
    && chown -R 10001:10001 /var/lib/plywise \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/personal-chess-tutor /usr/local/bin/personal-chess-tutor
COPY resources /opt/plywise/resources

LABEL org.opencontainers.image.title="Plywise C++ service" \
      org.opencontainers.image.description="Completed-game chess analysis and review service" \
      org.opencontainers.image.source="https://github.com/skcache/plywise"

ENV PCT_BIND_ADDRESS=0.0.0.0 \
    PCT_REQUIRE_AUTH=true \
    PCT_PORT=8787 \
    PCT_DATA_DIR=/var/lib/plywise \
    PCT_WEB_ROOT=/opt/plywise/web \
    PCT_STOCKFISH=/usr/games/stockfish \
    PCT_TACTICAL_CORPUS=/opt/plywise/resources/tactical-corpus.json \
    PCT_RESOURCE_CATALOG=/opt/plywise/resources/catalog.json \
    PCT_OPENING_BOOK=/opt/plywise/resources/openings.json \
    PCT_WORKERS=1 \
    PCT_MAX_PENDING=64 \
    PCT_RETRY_LIMIT=1

USER 10001:10001
WORKDIR /opt/plywise
VOLUME ["/var/lib/plywise"]
EXPOSE 8787
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=10s --timeout=3s --start-period=10s --retries=3 \
    CMD ["curl", "--fail", "--silent", "http://127.0.0.1:8787/api/ready"]

ENTRYPOINT ["/usr/local/bin/personal-chess-tutor"]
