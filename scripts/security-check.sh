#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

sh scripts/check-public-surface.sh

if [ -n "${PCT_SECURITY_BUILD_DIR:-}" ]; then
  BUILD_DIR=$PCT_SECURITY_BUILD_DIR
else
  BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/plywise-security.XXXXXX")
  trap 'cmake -E remove_directory "$BUILD_DIR"' EXIT INT TERM
fi

npm audit --prefix web --omit=dev
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DPCT_ENABLE_SANITIZERS=ON -DPCT_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD_DIR"
cmake -E env ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir "$BUILD_DIR" --output-on-failure

grep -q 'std::string bind_address{"127.0.0.1"}' include/pct/service/http_server.hpp
grep -q 'inet_pton(AF_INET, options_.bind_address.c_str()' src/service/http_server.cpp
grep -q 'chesscom_max_body_size = 10U \* 1024U \* 1024U' include/pct/import/chesscom_archive_client.hpp
grep -q 'max_body_size = 10 \* 1024 \* 1024' src/service/http_server.cpp
grep -q 'request_path.find("..")' src/service/http_server.cpp
grep -q 'CURLOPT_PROTOCOLS_STR, "https"' src/import/chesscom_archive_client.cpp
grep -q 'CURLOPT_FOLLOWLOCATION, 0L' src/import/chesscom_archive_client.cpp
grep -q 'validate_effective_endpoint' src/import/chesscom_archive_client.cpp
grep -q 'valid_websocket_origin' src/service/http_server.cpp
grep -q 'valid_configured_origin' src/service/http_server.cpp

if command -v clang-tidy >/dev/null 2>&1; then
  clang-tidy -p "$BUILD_DIR" \
    src/engine/pool.cpp src/storage/event_log.cpp src/import/chesscom_archive_client.cpp \
    src/app/ingest_manager.cpp src/service/http_server.cpp \
    --warnings-as-errors='clang-analyzer-*'
else
  /usr/bin/clang++ --analyze -std=c++20 -Iinclude src/engine/pool.cpp -o /tmp/pct-pool.plist
  /usr/bin/clang++ --analyze -std=c++20 -Iinclude src/storage/event_log.cpp -o /tmp/pct-storage.plist
  /usr/bin/clang++ --analyze -std=c++20 -Iinclude src/import/chesscom_archive_client.cpp -o /tmp/pct-chesscom.plist
  /usr/bin/clang++ --analyze -std=c++20 -Iinclude src/app/ingest_manager.cpp -o /tmp/pct-ingest.plist
  /usr/bin/clang++ --analyze -std=c++20 -Iinclude src/service/http_server.cpp -o /tmp/pct-http.plist
  printf '%s\n' "clang-tidy not installed; Clang Static Analyzer fallback passed."
fi
