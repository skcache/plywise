#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MIGRATIONS_DIR="$ROOT/db/migrations"

psql_args=(-X -v ON_ERROR_STOP=1)
if [[ -n "${DATABASE_URL:-}" ]]; then
  export PGDATABASE="$DATABASE_URL"
  unset DATABASE_URL
fi

checksum() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

psql "${psql_args[@]}" <<'SQL'
CREATE SCHEMA IF NOT EXISTS plywise;
CREATE TABLE IF NOT EXISTS plywise.schema_migrations (
    version text PRIMARY KEY,
    checksum text NOT NULL CHECK (checksum ~ '^[0-9a-f]{64}$'),
    applied_at timestamptz NOT NULL DEFAULT now()
);
SQL

shopt -s nullglob
migrations=("$MIGRATIONS_DIR"/*.sql)
if (( ${#migrations[@]} == 0 )); then
  echo "No PostgreSQL migrations found in $MIGRATIONS_DIR" >&2
  exit 1
fi

for migration in "${migrations[@]}"; do
  filename="$(basename "$migration")"
  version="${filename%%_*}"
  if [[ ! "$filename" =~ ^[0-9]{4}_[a-z0-9_]+\.sql$ ]]; then
    echo "Invalid migration filename: $filename" >&2
    exit 1
  fi

  migration_checksum="$(checksum "$migration")"
  existing_checksum="$(
    psql "${psql_args[@]}" --tuples-only --no-align \
      --command "SELECT checksum FROM plywise.schema_migrations WHERE version = '$version'"
  )"

  if [[ -n "$existing_checksum" ]]; then
    if [[ "$existing_checksum" != "$migration_checksum" ]]; then
      echo "Migration $version was modified after being applied" >&2
      exit 1
    fi
    continue
  fi

  psql "${psql_args[@]}" --single-transaction \
    --command "SELECT pg_advisory_xact_lock(724959731); LOCK TABLE plywise.schema_migrations IN EXCLUSIVE MODE" \
    --command "SET LOCAL search_path TO plywise, public" \
    --file "$migration" \
    --command "INSERT INTO plywise.schema_migrations (version, checksum) VALUES ('$version', '$migration_checksum') ON CONFLICT (version) DO NOTHING"

  stored_checksum="$(
    psql "${psql_args[@]}" --tuples-only --no-align \
      --command "SELECT checksum FROM plywise.schema_migrations WHERE version = '$version'"
  )"
  if [[ "$stored_checksum" != "$migration_checksum" ]]; then
    echo "Migration $version checksum does not match the applied version" >&2
    exit 1
  fi
done

echo "PostgreSQL schema is current (${#migrations[@]} migration(s))."
