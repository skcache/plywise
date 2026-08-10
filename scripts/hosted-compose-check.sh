#!/bin/sh
set -eu

fail() {
  printf 'hosted compose check failed: %s\n' "$1" >&2
  exit 1
}

require_value() {
  name=$1
  value=$2
  [ -n "$value" ] || fail "$name is required"
}

require_value PCT_API_DOMAIN "${PCT_API_DOMAIN:-}"
require_value PCT_POSTGRES_URL "${PCT_POSTGRES_URL:-}"
require_value PCT_SUPABASE_URL "${PCT_SUPABASE_URL:-}"
require_value PCT_TRUSTED_HOSTS "${PCT_TRUSTED_HOSTS:-}"
require_value PCT_ALLOWED_ORIGINS "${PCT_ALLOWED_ORIGINS:-}"

case "$PCT_API_DOMAIN" in
  */*|*:*|*@*|*' '*|*'	'*|*'\r'*|*'\n'*)
    fail "PCT_API_DOMAIN must be a hostname without a scheme, port, path, or credentials"
    ;;
esac
case "$PCT_API_DOMAIN" in
  *.*) ;;
  *) fail "PCT_API_DOMAIN must contain a DNS suffix" ;;
esac

case "$PCT_POSTGRES_URL" in
  *'sslmode=require'*|*'sslmode=verify-ca'*|*'sslmode=verify-full'*) ;;
  *) fail "PCT_POSTGRES_URL must require PostgreSQL TLS with sslmode=require, verify-ca, or verify-full" ;;
esac

case "$PCT_SUPABASE_URL" in
  https://*) ;;
  *) fail "PCT_SUPABASE_URL must use HTTPS" ;;
esac
supabase_authority=${PCT_SUPABASE_URL#https://}
case "$supabase_authority" in
  ''|*/*|*\?*|*#*|*@*|*' '*|*'	'*)
    fail "PCT_SUPABASE_URL must be a credential-free origin"
    ;;
esac

case ",$PCT_TRUSTED_HOSTS," in
  *,"$PCT_API_DOMAIN",*) ;;
  *) fail "PCT_TRUSTED_HOSTS must include PCT_API_DOMAIN" ;;
esac

old_ifs=$IFS
IFS=,
for origin in $PCT_ALLOWED_ORIGINS; do
  case "$origin" in
    https://*) ;;
    *) fail "every PCT_ALLOWED_ORIGINS entry must use HTTPS" ;;
  esac
  case "$origin" in
    https://|https://*/*|*\?*|*#*|*@*|*' '*|*'	'*)
      fail "PCT_ALLOWED_ORIGINS entries must be credential-free origins without paths"
      ;;
  esac
done
IFS=$old_ifs

command -v docker >/dev/null 2>&1 || fail "Docker is required"
docker compose version >/dev/null 2>&1 || fail "Docker Compose is required"

docker compose -f compose.yaml -f compose.hosted.yaml --profile edge config >/dev/null ||
  fail "Docker Compose rejected the hosted configuration"

printf 'hosted compose configuration passed for %s\n' "$PCT_API_DOMAIN"
