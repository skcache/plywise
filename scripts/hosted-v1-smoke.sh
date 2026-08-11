#!/bin/sh
set -eu

fail() {
  printf 'hosted V1 smoke failed: %s\n' "$1" >&2
  exit 1
}

require_value() {
  name=$1
  value=$2
  [ -n "$value" ] || fail "$name is required"
}

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${PCT_API_BASE_URL:-}
APP_ORIGIN=${PCT_APP_ORIGIN:-}
BEARER_TOKEN=${PCT_API_BEARER_TOKEN:-}
SECOND_BEARER_TOKEN=${PCT_V1_SECOND_BEARER_TOKEN:-}
PGN=${PCT_V1_PGN:-}
MAX_POLLS=${PCT_V1_MAX_POLLS:-90}

require_value PCT_API_BASE_URL "$BASE_URL"
require_value PCT_APP_ORIGIN "$APP_ORIGIN"
require_value PCT_API_BEARER_TOKEN "$BEARER_TOKEN"
require_value PCT_V1_PGN "$PGN"

case "$BASE_URL" in
  https://*|http://127.0.0.1:*|http://localhost:*) ;;
  *) fail "PCT_API_BASE_URL must be HTTPS or an explicit loopback URL" ;;
esac
case "$APP_ORIGIN" in
  https://*|http://127.0.0.1:*|http://localhost:*) ;;
  *) fail "PCT_APP_ORIGIN must be HTTPS or an explicit loopback URL" ;;
esac
case "$BASE_URL$APP_ORIGIN" in
  *' '*|*'?'*|*'#'*|*'@'*) fail "origins must not contain credentials, queries, fragments, or spaces" ;;
esac

PGN_BYTES=$(printf '%s' "$PGN" | wc -c | tr -d '[:space:]')
case "$PGN_BYTES" in
  ''|*[!0-9]*) fail "could not measure PCT_V1_PGN" ;;
esac
[ "$PGN_BYTES" -le 1048576 ] || fail "PCT_V1_PGN exceeds the 1 MiB request limit"

case "$MAX_POLLS" in
  ''|*[!0-9]*) fail "PCT_V1_MAX_POLLS must be an integer" ;;
esac
[ "$MAX_POLLS" -ge 1 ] && [ "$MAX_POLLS" -le 600 ] ||
  fail "PCT_V1_MAX_POLLS must be between 1 and 600"

TEMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/plywise-v1-smoke.XXXXXX")
cleanup() {
  rm -rf "$TEMP_DIR"
}
trap cleanup EXIT HUP INT TERM

request() {
  name=$1
  shift
  curl --silent --show-error --connect-timeout 5 --max-time 15 \
    --dump-header "$TEMP_DIR/$name.headers" \
    --output "$TEMP_DIR/$name.body" \
    --write-out '%{http_code}' "$@" >"$TEMP_DIR/$name.status" || true
}

status() {
  cat "$TEMP_DIR/$1.status"
}

assert_status() {
  name=$1
  expected=$2
  actual=$(status "$name")
  [ "$actual" = "$expected" ] || fail "$name returned HTTP $actual, expected HTTP $expected"
}

assert_json() {
  name=$1
  grep -Eiq '^content-type:[[:space:]]*application/json([;[:space:]]|$)' \
    "$TEMP_DIR/$name.headers" || fail "$name did not return application/json"
}

assert_body() {
  name=$1
  text=$2
  grep -Fq "$text" "$TEMP_DIR/$name.body" || fail "$name response was missing $text"
}

json_field() {
  file=$1
  path=$2
  node - "$file" "$path" <<'NODE'
const fs = require("fs");
const body = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
let value = body;
for (const part of process.argv[3].split(".")) value = value?.[part];
if (value === undefined || value === null) process.exit(1);
process.stdout.write(typeof value === "object" ? JSON.stringify(value) : String(value));
NODE
}

json_pgn() {
  node - "$PGN" <<'NODE'
process.stdout.write(JSON.stringify({ pgn: process.argv[2] }));
NODE
}

AUTH_HEADER="Authorization: Bearer $BEARER_TOKEN"
ORIGIN_HEADER="Origin: $APP_ORIGIN"

# Reuse the existing unauthenticated/CORS/WebSocket preflight before mutating storage.
PCT_API_BASE_URL="$BASE_URL" \
PCT_APP_ORIGIN="$APP_ORIGIN" \
PCT_API_BEARER_TOKEN="$BEARER_TOKEN" \
  "$ROOT/scripts/hosted-api-smoke.sh"

request import \
  -X POST \
  -H "$ORIGIN_HEADER" \
  -H "$AUTH_HEADER" \
  -H 'Content-Type: application/json' \
  --data-binary "$(json_pgn)" \
  "$BASE_URL/api/import"
assert_json import
case "$(status import)" in
  200|202) ;;
  *) fail "import returned HTTP $(status import), expected 200 or 202" ;;
esac
GAME_ID=$(json_field "$TEMP_DIR/import.body" game_id) || fail "import response did not include game_id"
[ -n "$GAME_ID" ] || fail "import returned an empty game_id"

request start_analysis \
  -X POST \
  -H "$ORIGIN_HEADER" \
  -H "$AUTH_HEADER" \
  -H 'Content-Type: application/json' \
  --data '{}' \
  "$BASE_URL/api/games/$GAME_ID/analysis"
assert_status start_analysis 202
assert_json start_analysis
JOB_ID=$(json_field "$TEMP_DIR/start_analysis.body" id) || fail "analysis response did not include a job id"
[ -n "$JOB_ID" ] || fail "analysis returned an empty job id"

attempt=0
status_name=
while :; do
  request job \
    -H "$ORIGIN_HEADER" \
    -H "$AUTH_HEADER" \
    "$BASE_URL/api/jobs/$JOB_ID"
  assert_status job 200
  assert_json job
  status_name=$(json_field "$TEMP_DIR/job.body" status) || fail "job response did not include status"
  progress_total=$(json_field "$TEMP_DIR/job.body" progress.total) ||
    fail "job response did not include bounded progress"
  case "$progress_total" in
    ''|*[!0-9]*) fail "job progress total was not numeric" ;;
  esac
  [ "$progress_total" -gt 0 ] || fail "job reported no analysis work"
  case "$status_name" in
    complete) break ;;
    failed|cancelled) fail "analysis job ended with status $status_name" ;;
    queued|running) ;;
    *) fail "analysis job returned unknown status $status_name" ;;
  esac
  attempt=$((attempt + 1))
  [ "$attempt" -lt "$MAX_POLLS" ] || fail "analysis did not complete within $MAX_POLLS polls"
  sleep 1
done

request review \
  -H "$ORIGIN_HEADER" \
  -H "$AUTH_HEADER" \
  "$BASE_URL/api/games/$GAME_ID"
assert_status review 200
assert_json review
assert_body review '"analysis_status":"complete"'
assert_body review '"analysis":'

# Read the same review through the list endpoint to prove the saved record is addressable after
# the job has completed, not just returned by the job response.
request library \
  -H "$ORIGIN_HEADER" \
  -H "$AUTH_HEADER" \
  "$BASE_URL/api/games?limit=100&offset=0"
assert_status library 200
assert_json library
json_field "$TEMP_DIR/library.body" games >/dev/null || fail "library response did not include games"
grep -Fq "\"id\":\"$GAME_ID\"" "$TEMP_DIR/library.body" ||
  fail "completed game was missing from the authenticated library"

if [ -n "$SECOND_BEARER_TOKEN" ]; then
  request other_owner \
    -H "$ORIGIN_HEADER" \
    -H "Authorization: Bearer $SECOND_BEARER_TOKEN" \
    "$BASE_URL/api/games/$GAME_ID"
  assert_json other_owner
  case "$(status other_owner)" in
    403|404) ;;
    *) fail "second owner returned HTTP $(status other_owner); expected 403 or 404" ;;
  esac
  printf '%s\n' 'cross-owner isolation passed'
else
  printf '%s\n' 'cross-owner isolation skipped (set PCT_V1_SECOND_BEARER_TOKEN to run it)'
fi

printf 'hosted V1 import -> analysis -> persistence smoke passed for %s\n' "$BASE_URL"
