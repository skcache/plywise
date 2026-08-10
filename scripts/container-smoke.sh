#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${PCT_CONTAINER_IMAGE:-plywise-api:smoke}
CONTAINER="plywise-smoke-$$"

cleanup() {
  docker rm --force "$CONTAINER" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

docker build --tag "$IMAGE" "$ROOT"
docker run --detach \
  --name "$CONTAINER" \
  --read-only \
  --cap-drop ALL \
  --security-opt no-new-privileges \
  --pids-limit 128 \
  --memory 768m \
  --cpus 1.5 \
  --tmpfs /tmp:rw,noexec,nosuid,size=64m \
  --tmpfs /var/lib/plywise:rw,noexec,nosuid,size=64m,uid=10001,gid=10001 \
  --env PCT_TRUSTED_HOSTS=api.plywise.test \
  --env PCT_ALLOWED_ORIGINS=https://app.plywise.test \
  --env PCT_REQUIRE_AUTH=false \
  --env PCT_ALLOW_INSECURE_REMOTE=true \
  --publish 127.0.0.1::8787 \
  "$IMAGE" >/dev/null

PORT=$(docker port "$CONTAINER" 8787/tcp | sed -n 's/.*://p')
BASE_URL="http://127.0.0.1:$PORT"

attempt=0
until curl --fail --silent "$BASE_URL/api/ready" >/dev/null; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 60 ]; then
    docker logs "$CONTAINER"
    exit 1
  fi
  sleep 1
done

[ "$(docker exec "$CONTAINER" id -u)" = "10001" ]
docker exec "$CONTAINER" test -s /licenses/stockfish-GPL-3.0.txt
docker exec "$CONTAINER" test -s /licenses/stockfish-debian-copyright.txt

CORS_RESPONSE=$(curl --fail --silent --include \
  --request OPTIONS \
  -H 'Host: api.plywise.test' \
  -H 'Origin: https://app.plywise.test' \
  -H 'Access-Control-Request-Method: GET' \
  "$BASE_URL/api/games")
printf '%s' "$CORS_RESPONSE" | grep -q 'HTTP/1.1 204 No Content'
printf '%s' "$CORS_RESPONSE" |
  grep -q 'Access-Control-Allow-Origin: https://app.plywise.test'

IMPORT_RESPONSE=$(curl --fail --silent \
  -H 'Content-Type: application/json' \
  --data '{"pgn":"[White \"Container\"]\n[Black \"Smoke\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0"}' \
  "$BASE_URL/api/import")
GAME_ID=$(printf '%s' "$IMPORT_RESPONSE" | node -e \
  'let body="";process.stdin.on("data",chunk=>body+=chunk);process.stdin.on("end",()=>process.stdout.write(JSON.parse(body).game_id));')
[ -n "$GAME_ID" ]

ANALYSIS_RESPONSE=$(curl --fail --silent \
  -H 'Content-Type: application/json' \
  --data '{}' \
  "$BASE_URL/api/games/$GAME_ID/analysis")
JOB_ID=$(printf '%s' "$ANALYSIS_RESPONSE" | node -e \
  'let body="";process.stdin.on("data",chunk=>body+=chunk);process.stdin.on("end",()=>process.stdout.write(String(JSON.parse(body).id)));')

attempt=0
STATUS=
until [ "$STATUS" = "complete" ]; do
  JOB_RESPONSE=$(curl --fail --silent "$BASE_URL/api/jobs/$JOB_ID")
  STATUS=$(printf '%s' "$JOB_RESPONSE" | node -e \
    'let body="";process.stdin.on("data",chunk=>body+=chunk);process.stdin.on("end",()=>process.stdout.write(JSON.parse(body).status));')
  if [ "$STATUS" = "failed" ] || [ "$STATUS" = "cancelled" ]; then
    printf '%s\n' "$JOB_RESPONSE"
    exit 1
  fi
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 90 ]; then
    docker logs "$CONTAINER"
    exit 1
  fi
  sleep 1
done

REVIEW=$(curl --fail --silent "$BASE_URL/api/games/$GAME_ID")
printf '%s' "$REVIEW" | node -e \
  'let body="";process.stdin.on("data",chunk=>body+=chunk);process.stdin.on("end",()=>{const review=JSON.parse(body);if(!review.analysis||review.analysis.moves.length!==2)process.exit(1);});'

SHUTDOWN_PAYLOAD=$(node -e \
  'const moves=[];for(let move=1;move<=40;move+=2){moves.push(`${move}. Nf3 Nf6 ${move+1}. Ng1 Ng8`)}process.stdout.write(JSON.stringify({pgn:`[White "Shutdown"]\n[Black "Cancellation"]\n[Result "1/2-1/2"]\n\n${moves.join(" ")} 1/2-1/2`}));')
SHUTDOWN_IMPORT=$(curl --fail --silent \
  -H 'Content-Type: application/json' \
  --data "$SHUTDOWN_PAYLOAD" \
  "$BASE_URL/api/import")
SHUTDOWN_GAME_ID=$(printf '%s' "$SHUTDOWN_IMPORT" | node -e \
  'let body="";process.stdin.on("data",chunk=>body+=chunk);process.stdin.on("end",()=>process.stdout.write(JSON.parse(body).game_id));')
curl --fail --silent \
  -H 'Content-Type: application/json' \
  --data '{}' \
  "$BASE_URL/api/games/$SHUTDOWN_GAME_ID/analysis" >/dev/null

sleep 1
docker stop --time 20 "$CONTAINER" >/dev/null
[ "$(docker inspect --format '{{.State.ExitCode}}' "$CONTAINER")" = "0" ]
docker logs "$CONTAINER" 2>&1 | grep -q 'shutdown requested; stopping HTTP admission'

printf '%s\n' "container import-to-review and graceful-shutdown smoke passed"
