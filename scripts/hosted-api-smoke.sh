#!/bin/sh
set -eu

fail() {
  printf 'hosted API preflight failed: %s\n' "$1" >&2
  exit 1
}

require_value() {
  name=$1
  value=$2
  [ -n "$value" ] || fail "$name is required"
}

BASE_URL=${PCT_API_BASE_URL:-}
APP_ORIGIN=${PCT_APP_ORIGIN:-}
BEARER_TOKEN=${PCT_API_BEARER_TOKEN:-}
EXPECT_AUTH=${PCT_EXPECT_AUTH_REQUIRED:-true}
ALLOW_LOCAL=${PCT_ALLOW_INSECURE_LOCAL:-false}

require_value PCT_API_BASE_URL "$BASE_URL"
require_value PCT_APP_ORIGIN "$APP_ORIGIN"

BASE_URL=${BASE_URL%/}
APP_ORIGIN=${APP_ORIGIN%/}

case "$EXPECT_AUTH" in
true|false) ;;
*) fail "PCT_EXPECT_AUTH_REQUIRED must be true or false" ;;
esac

case "$ALLOW_LOCAL" in
true|false) ;;
*) fail "PCT_ALLOW_INSECURE_LOCAL must be true or false" ;;
esac

local_base=false
case "$BASE_URL" in
https://*) ;;
http://127.0.0.1:*|http://localhost:*) local_base=true ;;
*) fail "PCT_API_BASE_URL must be an HTTPS origin (or an explicit loopback URL)" ;;
esac

case "$APP_ORIGIN" in
https://*) ;;
http://127.0.0.1:*|http://localhost:*) ;;
*) fail "PCT_APP_ORIGIN must be an HTTPS origin (or an explicit loopback URL)" ;;
esac

if [ "$local_base" = true ] && [ "$ALLOW_LOCAL" != true ]; then
  fail "loopback API checks require PCT_ALLOW_INSECURE_LOCAL=true"
fi

base_authority=${BASE_URL#*://}
app_authority=${APP_ORIGIN#*://}
case "$base_authority" in
''|*/*) fail "PCT_API_BASE_URL must be an origin without a path" ;;
esac
case "$app_authority" in
''|*/*) fail "PCT_APP_ORIGIN must be an origin without a path" ;;
esac
case "$BASE_URL$APP_ORIGIN" in
*\?*|*#*|*@*|*\ *)
  fail "API and app origins must not contain credentials, query strings, fragments, or spaces"
  ;;
esac

if [ "$EXPECT_AUTH" = true ]; then
  require_value PCT_API_BEARER_TOKEN "$BEARER_TOKEN"
fi

TEMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/plywise-api-smoke.XXXXXX")
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

request ready "$BASE_URL/api/ready"
assert_status ready 200
assert_json ready
assert_body ready '"status":"ready"'
assert_body ready '"api":"ready"'
assert_body ready '"storage":"ready"'
assert_body ready '"engine":"ready"'

request health -H "Origin: $APP_ORIGIN" "$BASE_URL/api/health"
assert_status health 200
assert_json health
assert_body health '"status":"ok"'
assert_body health '"service":"plywise-api"'
if [ "$EXPECT_AUTH" = true ]; then
  assert_body health '"auth_required":true'
else
  assert_body health '"auth_required":false'
fi

request preflight \
  -X OPTIONS \
  -H "Origin: $APP_ORIGIN" \
  -H 'Access-Control-Request-Method: GET' \
  -H 'Access-Control-Request-Headers: Authorization, Content-Type' \
  "$BASE_URL/api/games"
assert_status preflight 204
allow_origin=$(awk '
  tolower($0) ~ /^access-control-allow-origin:/ {
    line = $0
    sub(/^[^:]*:[[:space:]]*/, "", line)
    sub(/\r$/, "", line)
    print line
    exit
  }
' "$TEMP_DIR/preflight.headers")
[ "$allow_origin" = "$APP_ORIGIN" ] || fail "preflight did not echo the configured app origin"
grep -Eiq '^access-control-allow-headers:.*authorization' \
  "$TEMP_DIR/preflight.headers" || fail "preflight did not allow Authorization"
grep -Eiq '^access-control-allow-headers:.*content-type' \
  "$TEMP_DIR/preflight.headers" || fail "preflight did not allow Content-Type"

request unauthenticated_games -H "Origin: $APP_ORIGIN" "$BASE_URL/api/games"
assert_json unauthenticated_games
if [ "$EXPECT_AUTH" = true ]; then
  assert_status unauthenticated_games 401
  assert_body unauthenticated_games '"code":"auth_required"'
else
  assert_status unauthenticated_games 200
fi

if [ "$EXPECT_AUTH" = true ]; then
  request authenticated_games \
    -H "Origin: $APP_ORIGIN" \
    -H "Authorization: Bearer $BEARER_TOKEN" \
    "$BASE_URL/api/games"
  assert_status authenticated_games 200
  assert_json authenticated_games

  WS_HEADERS="$TEMP_DIR/websocket.headers"
  WS_BODY="$TEMP_DIR/websocket.body"
  set +e
  curl --silent --show-error --http1.1 --connect-timeout 5 --max-time 4 \
    --dump-header "$WS_HEADERS" --output "$WS_BODY" \
    -H "Origin: $APP_ORIGIN" \
    -H 'Connection: Upgrade' \
    -H 'Upgrade: websocket' \
    -H 'Sec-WebSocket-Version: 13' \
    -H 'Sec-WebSocket-Key: cGx5d2lzZS1wcmVmbGlnaHQ=' \
    -H "Sec-WebSocket-Protocol: plywise-auth, $BEARER_TOKEN" \
    "$BASE_URL/ws"
  websocket_curl_status=$?
  set -e
  case "$websocket_curl_status" in
  0|28|52|56) ;;
  *) fail "WebSocket handshake request failed with curl status $websocket_curl_status" ;;
  esac
  grep -Fq 'HTTP/1.1 101 Switching Protocols' "$WS_HEADERS" ||
    fail "WebSocket endpoint did not complete an authenticated handshake"
  grep -Eiq '^sec-websocket-protocol:[[:space:]]*plywise-auth[[:space:]]*$' "$WS_HEADERS" ||
    fail "WebSocket endpoint did not select the Plywise auth protocol"
fi

printf 'hosted API preflight passed for %s\n' "$BASE_URL"
