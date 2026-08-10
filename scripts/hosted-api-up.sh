#!/bin/sh
set -eu

fail() {
  printf 'hosted API launch failed: %s\n' "$1" >&2
  exit 1
}

require_value() {
  name=$1
  value=$2
  [ -n "$value" ] || fail "$name is required"
}

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
IMAGE=${PCT_API_IMAGE:-}
DOMAIN=${PCT_API_DOMAIN:-}
HEALTH_URL=${PCT_API_HEALTH_URL:-}
TIMEOUT=${PCT_API_START_TIMEOUT_SECONDS:-90}

require_value PCT_API_IMAGE "$IMAGE"
require_value PCT_API_DOMAIN "$DOMAIN"

case "$IMAGE" in
  ghcr.io/skcache/plywise-api@sha256:*) ;;
  *) fail "PCT_API_IMAGE must be the official API image pinned by digest (ghcr.io/skcache/plywise-api@sha256:...)" ;;
esac
digest=${IMAGE#*@sha256:}
printf '%s' "$digest" | grep -Eq '^[[:xdigit:]]{64}$' ||
  fail "PCT_API_IMAGE must contain a 64-character SHA-256 digest"

case "$TIMEOUT" in
  ''|*[!0-9]*) fail "PCT_API_START_TIMEOUT_SECONDS must be an integer" ;;
esac
[ "$TIMEOUT" -ge 10 ] && [ "$TIMEOUT" -le 600 ] ||
  fail "PCT_API_START_TIMEOUT_SECONDS must be between 10 and 600"

if [ -z "$HEALTH_URL" ]; then
  HEALTH_URL="https://$DOMAIN/api/ready"
fi
case "$HEALTH_URL" in
  https://*) ;;
  *) fail "PCT_API_HEALTH_URL must use HTTPS" ;;
esac
case "$HEALTH_URL" in
  *' '*|*@*|*\?*|*\#*) fail "PCT_API_HEALTH_URL must not contain credentials, queries, fragments, or spaces" ;;
esac

command -v docker >/dev/null 2>&1 || fail "Docker is required"
docker compose version >/dev/null 2>&1 || fail "Docker Compose is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

compose() {
  docker compose \
    -f "$ROOT/compose.yaml" \
    -f "$ROOT/compose.hosted.yaml" \
    --profile edge "$@"
}

# Validate every hosted secret/origin/TLS invariant before pulling or replacing a
# running service. The check deliberately prints only the API hostname.
PCT_API_DOMAIN="$DOMAIN" "$ROOT/scripts/hosted-compose-check.sh"

# Pull the exact image requested by the operator. The digest requirement above
# prevents a mutable tag from silently changing the production binary.
compose pull api edge >/dev/null
compose up --detach api edge >/dev/null

attempt=0
while :; do
  if response=$(curl --fail --silent --show-error --connect-timeout 5 --max-time 10 \
    "$HEALTH_URL" 2>/dev/null) &&
    printf '%s' "$response" | grep -Fq '"status":"ready"'; then
    printf 'hosted API is running at %s\n' "$DOMAIN"
    exit 0
  fi

  attempt=$((attempt + 1))
  if [ "$attempt" -ge "$TIMEOUT" ]; then
    compose ps >&2 || true
    fail "readiness check did not pass within ${TIMEOUT}s"
  fi
  sleep 1
done
