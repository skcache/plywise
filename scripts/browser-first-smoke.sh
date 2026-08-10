#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/plywise-browser-first.XXXXXX")
BUILD_DIR="$TMP/build"
DATA_DIR="$TMP/data"
API_PORT=${PCT_BROWSER_API_PORT:-18787}
WEB_PORT=${PCT_BROWSER_WEB_PORT:-4177}
STOCKFISH=${PCT_STOCKFISH:-$(command -v stockfish 2>/dev/null || true)}
API_URL="http://127.0.0.1:$API_PORT"
WEB_URL="http://127.0.0.1:$WEB_PORT"
API_LOG="$TMP/api.log"
WEB_LOG="$TMP/web.log"

API_PID=
WEB_PID=
cleanup() {
  if [ -n "$WEB_PID" ] && kill -0 "$WEB_PID" 2>/dev/null; then
    kill "$WEB_PID" 2>/dev/null || true
    wait "$WEB_PID" 2>/dev/null || true
  fi
  if [ -n "$API_PID" ] && kill -0 "$API_PID" 2>/dev/null; then
    kill "$API_PID" 2>/dev/null || true
    wait "$API_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

if [ -z "$STOCKFISH" ] || [ ! -x "$STOCKFISH" ]; then
  echo "browser-first smoke needs Stockfish; set PCT_STOCKFISH to an executable" >&2
  exit 1
fi
command -v cmake >/dev/null 2>&1 || { echo "browser-first smoke needs cmake" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "browser-first smoke needs curl" >&2; exit 1; }
command -v node >/dev/null 2>&1 || { echo "browser-first smoke needs node" >&2; exit 1; }

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DPCT_BUILD_BENCHMARKS=OFF \
  -DPCT_ENABLE_POSTGRES_ADAPTER=OFF \
  -DPCT_ENABLE_OIDC_AUTH=OFF \
  -DPCT_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD_DIR" --target personal-chess-tutor

# A remote unauthenticated bind must never be accepted by accident. The explicit opt-in is used
# only for the isolated container smoke; this check protects the browser-first local path.
set +e
PCT_BIND_ADDRESS=0.0.0.0 \
PCT_PORT="$API_PORT" \
PCT_REQUIRE_AUTH=false \
"$BUILD_DIR/personal-chess-tutor" \
  --data-dir "$TMP/guard-data" \
  --web-root "$ROOT/web/dist" \
  --stockfish "$STOCKFISH" \
  --no-tactical-corpus >"$TMP/remote-guard.log" 2>&1 &
GUARD_PID=$!
sleep 1
if kill -0 "$GUARD_PID" 2>/dev/null; then
  kill "$GUARD_PID" 2>/dev/null || true
  wait "$GUARD_PID" 2>/dev/null || true
  echo "remote unauthenticated bind was not rejected" >&2
  cat "$TMP/remote-guard.log" >&2
  exit 1
fi
wait "$GUARD_PID" 2>/dev/null
GUARD_STATUS=$?
set -e
[ "$GUARD_STATUS" -ne 0 ]
grep -q "remote HTTP binds require" "$TMP/remote-guard.log"

# A remote authenticated bind must also fail fast when its hosted identity/storage or browser
# allowlists are missing. Starting in that state would leave private routes at 503 or reject every
# browser origin while looking healthy at the process level.
set +e
PCT_BIND_ADDRESS=0.0.0.0 \
PCT_PORT="$API_PORT" \
PCT_REQUIRE_AUTH=true \
"$BUILD_DIR/personal-chess-tutor" \
  --data-dir "$TMP/misconfigured-hosted-data" \
  --web-root "$ROOT/web/dist" \
  --stockfish "$STOCKFISH" \
  --no-tactical-corpus >"$TMP/hosted-config-guard.log" 2>&1
HOSTED_GUARD_STATUS=$?
set -e
[ "$HOSTED_GUARD_STATUS" -ne 0 ]
grep -q "authenticated remote HTTP binds require PCT_POSTGRES_URL" "$TMP/hosted-config-guard.log"

mkdir -p "$DATA_DIR"
"$BUILD_DIR/personal-chess-tutor" \
  --bind-address 127.0.0.1 \
  --port "$API_PORT" \
  --data-dir "$DATA_DIR" \
  --web-root "$ROOT/web/dist" \
  --stockfish "$STOCKFISH" \
  --workers 1 \
  --max-pending 8 \
  --no-tactical-corpus >"$API_LOG" 2>&1 &
API_PID=$!

attempt=0
until curl --fail --silent "$API_URL/api/ready" >/dev/null; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 30 ]; then
    cat "$API_LOG" >&2
    exit 1
  fi
  sleep 1
done
READY_HEADERS=$(curl --fail --silent --include -H "Origin: $WEB_URL" "$API_URL/api/ready")
printf '%s' "$READY_HEADERS" | grep -qi 'Cache-Control: no-store'

(
  cd "$ROOT/web"
  VITE_PLYWISE_API_ORIGIN="$API_URL" \
    npm run dev -- --host 127.0.0.1 --port "$WEB_PORT"
) >"$WEB_LOG" 2>&1 &
WEB_PID=$!

attempt=0
until curl --fail --silent "$WEB_URL/" >/dev/null; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 30 ]; then
    cat "$WEB_LOG" >&2
    exit 1
  fi
  sleep 1
done

# Use the Vite origin in the request headers to exercise the same CORS path a browser uses.
CORS_RESPONSE=$(curl --fail --silent --include \
  --request OPTIONS \
  -H "Host: 127.0.0.1:$API_PORT" \
  -H "Origin: $WEB_URL" \
  -H 'Access-Control-Request-Method: GET' \
  "$API_URL/api/games")
printf '%s' "$CORS_RESPONSE" | grep -q 'HTTP/1.1 204 No Content'
printf '%s' "$CORS_RESPONSE" | grep -q "Access-Control-Allow-Origin: $WEB_URL"

IMPORT_RESPONSE=$(curl --fail --silent \
  -H "Origin: $WEB_URL" \
  -H 'Content-Type: application/json' \
  --data '{"pgn":"[White \"Browser\"]\n[Black \"Smoke\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0"}' \
  "$API_URL/api/import")
GAME_ID=$(printf '%s' "$IMPORT_RESPONSE" | node -e \
  'let body="";process.stdin.on("data",chunk=>body+=chunk);process.stdin.on("end",()=>process.stdout.write(JSON.parse(body).game_id));')
[ -n "$GAME_ID" ]

ANALYSIS_RESPONSE=$(curl --fail --silent \
  -H "Origin: $WEB_URL" \
  -H 'Content-Type: application/json' \
  --data '{}' \
  "$API_URL/api/games/$GAME_ID/analysis")
JOB_ID=$(printf '%s' "$ANALYSIS_RESPONSE" | node -e \
  'let body="";process.stdin.on("data",chunk=>body+=chunk);process.stdin.on("end",()=>process.stdout.write(String(JSON.parse(body).id)));')

attempt=0
STATUS=
until [ "$STATUS" = "complete" ]; do
  JOB_RESPONSE=$(curl --fail --silent -H "Origin: $WEB_URL" "$API_URL/api/jobs/$JOB_ID")
  STATUS=$(printf '%s' "$JOB_RESPONSE" | node -e \
    'let body="";process.stdin.on("data",chunk=>body+=chunk);process.stdin.on("end",()=>process.stdout.write(JSON.parse(body).status));')
  if [ "$STATUS" = "failed" ] || [ "$STATUS" = "cancelled" ]; then
    printf '%s\n' "$JOB_RESPONSE" >&2
    exit 1
  fi
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 90 ]; then
    cat "$API_LOG" >&2
    exit 1
  fi
  sleep 1
done

REVIEW=$(curl --fail --silent -H "Origin: $WEB_URL" "$API_URL/api/games/$GAME_ID")
printf '%s' "$REVIEW" | node -e \
  'let body="";process.stdin.on("data",chunk=>body+=chunk);process.stdin.on("end",()=>{const game=JSON.parse(body);if(!game.analysis||game.analysis.moves.length!==2)process.exit(1);});'

printf '%s\n' "browser-first C++ API + Vite origin smoke passed"
