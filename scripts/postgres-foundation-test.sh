#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_database="plywise_it_${PPID}_${RANDOM}"
restore_database="${test_database}_restore"
dump_directory="$(mktemp -d)"

if [[ ! "$test_database" =~ ^plywise_it_[0-9]+_[0-9]+$ ]]; then
  echo "Refusing to use unexpected integration database name" >&2
  exit 1
fi

cleanup() {
  dropdb --if-exists "$restore_database" >/dev/null 2>&1 || true
  dropdb --if-exists "$test_database" >/dev/null 2>&1 || true
  rm -rf "$dump_directory"
}
trap cleanup EXIT

for command in createdb dropdb pg_dump pg_restore psql; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "$command is required for the PostgreSQL integration test" >&2
    exit 1
  fi
done

expect_failure() {
  local description="$1"
  local statement="$2"
  if PGDATABASE="$test_database" psql -X -v ON_ERROR_STOP=1 --command "$statement" \
      >/dev/null 2>&1; then
    echo "Expected database constraint to reject: $description" >&2
    exit 1
  fi
}

createdb "$test_database"
PGDATABASE="$test_database" "$ROOT/scripts/postgres-migrate.sh"
PGDATABASE="$test_database" "$ROOT/scripts/postgres-migrate.sh"

PGDATABASE="$test_database" psql -X -v ON_ERROR_STOP=1 <<'SQL'
SET search_path TO plywise, public;

INSERT INTO owners (owner_kind, owner_id) VALUES
    ('account', 'account_a'),
    ('account', 'account_b'),
    ('account', 'account_c');
INSERT INTO owners (owner_kind, owner_id, expires_at)
    VALUES ('guest', 'guest_a', now() + interval '24 hours');

INSERT INTO accounts (id, auth_provider, auth_subject) VALUES
    ('account_a', 'test', 'subject_a'),
    ('account_b', 'test', 'subject_b'),
    ('account_c', 'test', 'subject_c');
INSERT INTO guest_sessions (id, token_hash, expires_at)
    VALUES ('guest_a', decode(repeat('aa', 32), 'hex'), now() + interval '24 hours');

INSERT INTO games (id, canonical_hash, normalized_pgn) VALUES
    ('game_a', decode(repeat('11', 32), 'hex'), '[Result "1-0"] 1. e4 e5 1-0'),
    ('game_b', decode(repeat('22', 32), 'hex'), '[Result "0-1"] 1. d4 d5 0-1');
INSERT INTO game_owners (game_id, owner_kind, owner_id, source_kind, source_key) VALUES
    ('game_a', 'account', 'account_a', 'manual_pgn', 'import_a'),
    ('game_a', 'account', 'account_b', 'manual_pgn', 'import_a');

INSERT INTO analysis_runs (
    id, game_id, owner_kind, owner_id, idempotency_key, source, engine_build,
    profile_version, classifier_version, compatibility_key, status, completed_at
) VALUES (
    'run_a1', 'game_a', 'account', 'account_a', 'analysis_a1', 'server', 'stockfish-test',
    'balanced-v1', 'classifier-v1', 'review-v1', 'completed', now()
);
INSERT INTO analysis_runs (
    id, game_id, owner_kind, owner_id, idempotency_key, source, engine_build,
    profile_version, classifier_version, compatibility_key, status, completed_at, supersedes_id
) VALUES (
    'run_a2', 'game_a', 'account', 'account_a', 'analysis_a2', 'server', 'stockfish-test',
    'balanced-v2', 'classifier-v1', 'review-v1', 'completed', now(), 'run_a1'
);
INSERT INTO analysis_heads (
    game_id, owner_kind, owner_id, compatibility_key, run_id
) VALUES ('game_a', 'account', 'account_a', 'review-v1', 'run_a2');
INSERT INTO analysis_positions (
    run_id, game_id, owner_kind, owner_id, ply, sequence, canonical_fen_hash,
    depth, nodes, time_ms, observation_json
) VALUES (
    'run_a2', 'game_a', 'account', 'account_a', 0, 0,
    decode(repeat('33', 32), 'hex'), 12, 1200, 25, '{"best_move":"e2e4"}'::jsonb
);
INSERT INTO move_assessments (
    run_id, game_id, owner_kind, owner_id, ply, assessment_json
) VALUES ('run_a2', 'game_a', 'account', 'account_a', 0, '{"classification":"best"}'::jsonb);
INSERT INTO reviews (run_id, game_id, owner_kind, owner_id, review_json)
    VALUES ('run_a2', 'game_a', 'account', 'account_a', '{"accuracy":91.2}'::jsonb);
INSERT INTO analysis_jobs (
    id, run_id, game_id, owner_kind, owner_id, idempotency_key, status
) VALUES ('job_a2', 'run_a2', 'game_a', 'account', 'account_a', 'job_a2', 'completed');
INSERT INTO job_events (
    job_id, run_id, owner_kind, owner_id, sequence, stage, completed_units, total_units
) VALUES ('job_a2', 'run_a2', 'account', 'account_a', 0, 'completed', 1, 1);

INSERT INTO intelligence_evidence (
    id, owner_kind, owner_id, run_id, game_id, ply, evidence_kind, model_version, evidence_json
) VALUES (
    'evidence_a', 'account', 'account_a', 'run_a2', 'game_a', 0,
    'piece_safety', 'evidence-v1', '{"sample_size":1}'::jsonb
);
INSERT INTO practice_items (
    id, owner_kind, owner_id, evidence_id, state, schedule_version
) VALUES ('practice_a', 'account', 'account_a', 'evidence_a', 'queued', 'schedule-v1');
INSERT INTO practice_outcomes (
    id, practice_item_id, owner_kind, owner_id, result, response_time_ms, hint_level
) VALUES ('outcome_a', 'practice_a', 'account', 'account_a', 'correct', 1500, 0);
INSERT INTO user_settings (owner_kind, owner_id, settings_json)
    VALUES ('account', 'account_a', '{"theme":"system"}'::jsonb);
INSERT INTO account_data_requests (
    id, owner_kind, owner_id, request_kind, idempotency_key, status
) VALUES ('export_a', 'account', 'account_a', 'export', 'export_once', 'requested');
INSERT INTO account_deletion_receipts (
    request_id, receipt_token_hash, status, completed_at, expires_at
) VALUES (
    'delete_a', decode(repeat('66', 32), 'hex'), 'completed', now(), now() + interval '30 days'
);
INSERT INTO idempotency_records (
    owner_kind, owner_id, operation, idempotency_key, request_hash, expires_at
) VALUES (
    'account', 'account_a', 'import', 'import_once', decode(repeat('44', 32), 'hex'),
    now() + interval '24 hours'
);
INSERT INTO outbox_events (
    owner_kind, owner_id, aggregate_kind, aggregate_id, event_kind, payload_json
) VALUES ('account', 'account_a', 'analysis', 'run_a2', 'analysis.completed', '{}'::jsonb);
SQL

expect_failure "duplicate import key for one owner" \
  "INSERT INTO plywise.game_owners (game_id, owner_kind, owner_id, source_kind, source_key) VALUES ('game_b', 'account', 'account_a', 'manual_pgn', 'import_a')"
expect_failure "analysis without game ownership" \
  "INSERT INTO plywise.analysis_runs (id, game_id, owner_kind, owner_id, idempotency_key, source, engine_build, profile_version, classifier_version, compatibility_key, status) VALUES ('run_c1', 'game_a', 'account', 'account_c', 'analysis_c1', 'server', 'stockfish-test', 'balanced-v1', 'classifier-v1', 'review-v1', 'created')"
expect_failure "duplicate analysis idempotency key" \
  "INSERT INTO plywise.analysis_runs (id, game_id, owner_kind, owner_id, idempotency_key, source, engine_build, profile_version, classifier_version, compatibility_key, status) VALUES ('run_a3', 'game_a', 'account', 'account_a', 'analysis_a2', 'server', 'stockfish-test', 'balanced-v2', 'classifier-v1', 'review-v1', 'created')"
expect_failure "cross-owner superseded analysis" \
  "INSERT INTO plywise.analysis_runs (id, game_id, owner_kind, owner_id, idempotency_key, source, engine_build, profile_version, classifier_version, compatibility_key, status, supersedes_id) VALUES ('run_b1', 'game_a', 'account', 'account_b', 'analysis_b1', 'server', 'stockfish-test', 'balanced-v1', 'classifier-v1', 'review-v1', 'created', 'run_a1')"
expect_failure "position with mismatched owner" \
  "INSERT INTO plywise.analysis_positions (run_id, game_id, owner_kind, owner_id, ply, sequence, canonical_fen_hash, depth, nodes, time_ms, observation_json) VALUES ('run_a2', 'game_a', 'account', 'account_b', 1, 0, decode(repeat('55', 32), 'hex'), 12, 100, 10, '{}'::jsonb)"
expect_failure "practice item using another owner's evidence" \
  "INSERT INTO plywise.practice_items (id, owner_kind, owner_id, evidence_id, state, schedule_version) VALUES ('practice_b', 'account', 'account_b', 'evidence_a', 'queued', 'schedule-v1')"

pg_dump --format=custom --file "$dump_directory/plywise.dump" "$test_database"
createdb "$restore_database"
pg_restore --no-owner --no-privileges --dbname "$restore_database" \
  "$dump_directory/plywise.dump"
PGDATABASE="$restore_database" "$ROOT/scripts/postgres-migrate.sh"

restored_runs="$(
  PGDATABASE="$restore_database" psql -X --tuples-only --no-align \
    --command "SELECT count(*) FROM plywise.analysis_runs"
)"
restored_migrations="$(
  PGDATABASE="$restore_database" psql -X --tuples-only --no-align \
    --command "SELECT count(*) FROM plywise.schema_migrations"
)"
if [[ "$restored_runs" != "2" || "$restored_migrations" != "1" ]]; then
  echo "Restored database did not preserve the qualified schema and data" >&2
  exit 1
fi

PGDATABASE="$restore_database" psql -X -v ON_ERROR_STOP=1 \
  --command "UPDATE plywise.schema_migrations SET checksum = repeat('0', 64) WHERE version = '0001'" \
  >/dev/null
if PGDATABASE="$restore_database" "$ROOT/scripts/postgres-migrate.sh" >/dev/null 2>&1; then
  echo "Migration runner accepted a changed applied migration" >&2
  exit 1
fi

PGDATABASE="$test_database" psql -X -v ON_ERROR_STOP=1 <<'SQL'
DELETE FROM plywise.owners
WHERE owner_kind = 'account' AND owner_id = 'account_a';

DO $test$
BEGIN
    IF EXISTS (
        SELECT 1 FROM plywise.analysis_runs
        WHERE owner_kind = 'account' AND owner_id = 'account_a'
    ) THEN
        RAISE EXCEPTION 'account analysis data was not deleted';
    END IF;
    IF EXISTS (
        SELECT 1 FROM plywise.user_settings
        WHERE owner_kind = 'account' AND owner_id = 'account_a'
    ) THEN
        RAISE EXCEPTION 'account settings were not deleted';
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM plywise.game_owners
        WHERE owner_kind = 'account' AND owner_id = 'account_b'
    ) THEN
        RAISE EXCEPTION 'deleting one account affected another owner';
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM plywise.account_deletion_receipts
        WHERE request_id = 'delete_a'
    ) THEN
        RAISE EXCEPTION 'account deletion receipt did not survive private data removal';
    END IF;
END
$test$;
SQL

echo "PostgreSQL ownership, idempotency, deletion, and restore checks passed."
