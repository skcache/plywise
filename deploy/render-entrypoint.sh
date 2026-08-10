#!/usr/bin/env bash
set -euo pipefail

# Render supplies the hosted database URL as a secret. Run the checked-in,
# checksum-verified migrations before admitting API traffic. A missing or
# failed migration is intentionally fatal: serving an API against a partial
# schema would turn authorization and persistence failures into misleading
# bearer-token errors.
if [[ -n "${PCT_POSTGRES_URL:-}" ]]; then
  DATABASE_URL="$PCT_POSTGRES_URL" /opt/plywise/scripts/postgres-migrate.sh
fi

exec /usr/local/bin/personal-chess-tutor "$@"
