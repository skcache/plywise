#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-$ROOT/build-postgres/pct-tests}"
test_database="plywise_repo_it_${PPID}_${RANDOM}"

# Keep local peer-authenticated PostgreSQL usable while honoring CI's explicit
# connection settings.
export PGUSER="${PGUSER:-$(id -un)}"

cleanup() {
  dropdb --if-exists "$test_database" >/dev/null 2>&1 || true
}
trap cleanup EXIT

for command in createdb dropdb psql; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "$command is required for the PostgreSQL repository test" >&2
    exit 1
  fi
done

createdb "$test_database"
PGDATABASE="$test_database" "$ROOT/scripts/postgres-migrate.sh" >/dev/null
PGDATABASE="$test_database" psql -X -v ON_ERROR_STOP=1 >/dev/null <<'SQL'
SET search_path TO plywise, public;
INSERT INTO owners (owner_kind, owner_id) VALUES
    ('account', 'account_test_a'),
    ('account', 'account_test_b');
INSERT INTO owners (owner_kind, owner_id, expires_at) VALUES
    ('guest', 'guest_test', now() + interval '1 hour'),
    ('guest', 'guest_expired', now() - interval '1 hour');
INSERT INTO accounts (id, auth_provider, auth_subject) VALUES
    ('account_test_a', 'test', 'subject_a'),
    ('account_test_b', 'test', 'subject_b');
SQL

test_url="host=${PGHOST:-127.0.0.1} port=${PGPORT:-5432} user=$PGUSER dbname=$test_database"
PCT_POSTGRES_TEST_URL="$test_url" "$binary"
