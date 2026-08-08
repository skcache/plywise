CREATE TABLE IF NOT EXISTS owners (
    owner_kind text NOT NULL CHECK (owner_kind IN ('account', 'guest')),
    owner_id text NOT NULL CHECK (length(owner_id) BETWEEN 1 AND 256),
    created_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz,
    PRIMARY KEY (owner_kind, owner_id),
    CHECK (
        (owner_kind = 'account' AND expires_at IS NULL) OR
        (owner_kind = 'guest' AND expires_at IS NOT NULL)
    )
);

CREATE TABLE IF NOT EXISTS accounts (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    owner_kind text NOT NULL DEFAULT 'account' CHECK (owner_kind = 'account'),
    auth_provider text NOT NULL CHECK (length(auth_provider) BETWEEN 1 AND 128),
    auth_subject text NOT NULL CHECK (length(auth_subject) BETWEEN 1 AND 512),
    email text,
    created_at timestamptz NOT NULL DEFAULT now(),
    deletion_requested_at timestamptz,
    UNIQUE (owner_kind, id),
    UNIQUE (auth_provider, auth_subject),
    FOREIGN KEY (owner_kind, id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS guest_sessions (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    owner_kind text NOT NULL DEFAULT 'guest' CHECK (owner_kind = 'guest'),
    token_hash bytea NOT NULL UNIQUE CHECK (octet_length(token_hash) = 32),
    created_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL,
    claimed_by_owner_kind text CHECK (claimed_by_owner_kind IS NULL OR claimed_by_owner_kind = 'account'),
    claimed_by_account_id text,
    claimed_at timestamptz,
    UNIQUE (owner_kind, id),
    FOREIGN KEY (owner_kind, id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE,
    FOREIGN KEY (claimed_by_owner_kind, claimed_by_account_id)
        REFERENCES accounts (owner_kind, id),
    CHECK (
        (claimed_by_owner_kind IS NULL AND claimed_by_account_id IS NULL AND claimed_at IS NULL) OR
        (claimed_by_owner_kind = 'account' AND claimed_by_account_id IS NOT NULL AND claimed_at IS NOT NULL)
    )
);

CREATE TABLE IF NOT EXISTS games (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    canonical_hash bytea NOT NULL UNIQUE CHECK (octet_length(canonical_hash) = 32),
    normalized_pgn text NOT NULL CHECK (length(normalized_pgn) > 0),
    metadata_json jsonb NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(metadata_json) = 'object'),
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS game_owners (
    game_id text NOT NULL REFERENCES games (id) ON DELETE CASCADE,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    imported_at timestamptz NOT NULL DEFAULT now(),
    source_kind text NOT NULL CHECK (length(source_kind) BETWEEN 1 AND 64),
    source_key text,
    provenance_json jsonb NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(provenance_json) = 'object'),
    PRIMARY KEY (game_id, owner_kind, owner_id),
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE
);

CREATE UNIQUE INDEX IF NOT EXISTS game_owners_source_key_unique
    ON game_owners (owner_kind, owner_id, source_kind, source_key)
    WHERE source_key IS NOT NULL;

CREATE INDEX IF NOT EXISTS game_owners_owner_imported_index
    ON game_owners (owner_kind, owner_id, imported_at DESC);

CREATE TABLE IF NOT EXISTS analysis_runs (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    game_id text NOT NULL,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    idempotency_key text NOT NULL CHECK (length(idempotency_key) BETWEEN 1 AND 256),
    source text NOT NULL CHECK (source IN ('browser', 'server')),
    engine_build text NOT NULL CHECK (length(engine_build) BETWEEN 1 AND 256),
    engine_hash bytea CHECK (engine_hash IS NULL OR octet_length(engine_hash) = 32),
    profile_version text NOT NULL CHECK (length(profile_version) BETWEEN 1 AND 128),
    classifier_version text NOT NULL CHECK (length(classifier_version) BETWEEN 1 AND 128),
    compatibility_key text NOT NULL CHECK (length(compatibility_key) BETWEEN 1 AND 256),
    status text NOT NULL CHECK (
        status IN ('created', 'collecting', 'validating', 'classifying', 'completed', 'cancelled', 'failed')
    ),
    created_at timestamptz NOT NULL DEFAULT now(),
    completed_at timestamptz,
    supersedes_id text,
    UNIQUE (id, game_id, owner_kind, owner_id),
    UNIQUE (owner_kind, owner_id, idempotency_key),
    FOREIGN KEY (game_id, owner_kind, owner_id)
        REFERENCES game_owners (game_id, owner_kind, owner_id) ON DELETE CASCADE,
    FOREIGN KEY (supersedes_id, game_id, owner_kind, owner_id)
        REFERENCES analysis_runs (id, game_id, owner_kind, owner_id),
    CHECK (supersedes_id IS NULL OR supersedes_id <> id),
    CHECK ((status = 'completed') = (completed_at IS NOT NULL))
);

CREATE INDEX IF NOT EXISTS analysis_runs_owner_created_index
    ON analysis_runs (owner_kind, owner_id, created_at DESC);

CREATE TABLE IF NOT EXISTS analysis_heads (
    game_id text NOT NULL,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    compatibility_key text NOT NULL,
    run_id text NOT NULL,
    revision bigint NOT NULL DEFAULT 1 CHECK (revision > 0),
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (game_id, owner_kind, owner_id, compatibility_key),
    FOREIGN KEY (run_id, game_id, owner_kind, owner_id)
        REFERENCES analysis_runs (id, game_id, owner_kind, owner_id),
    FOREIGN KEY (game_id, owner_kind, owner_id)
        REFERENCES game_owners (game_id, owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS analysis_positions (
    run_id text NOT NULL,
    game_id text NOT NULL,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    ply integer NOT NULL CHECK (ply >= 0),
    sequence integer NOT NULL CHECK (sequence >= 0),
    canonical_fen_hash bytea NOT NULL CHECK (octet_length(canonical_fen_hash) = 32),
    depth integer NOT NULL CHECK (depth >= 0),
    nodes bigint NOT NULL CHECK (nodes >= 0),
    time_ms bigint NOT NULL CHECK (time_ms >= 0),
    observation_json jsonb NOT NULL CHECK (jsonb_typeof(observation_json) = 'object'),
    validated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (run_id, ply, sequence),
    FOREIGN KEY (run_id, game_id, owner_kind, owner_id)
        REFERENCES analysis_runs (id, game_id, owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS move_assessments (
    run_id text NOT NULL,
    game_id text NOT NULL,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    ply integer NOT NULL CHECK (ply >= 0),
    assessment_json jsonb NOT NULL CHECK (jsonb_typeof(assessment_json) = 'object'),
    PRIMARY KEY (run_id, ply),
    FOREIGN KEY (run_id, game_id, owner_kind, owner_id)
        REFERENCES analysis_runs (id, game_id, owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS reviews (
    run_id text PRIMARY KEY,
    game_id text NOT NULL,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    review_json jsonb NOT NULL CHECK (jsonb_typeof(review_json) = 'object'),
    created_at timestamptz NOT NULL DEFAULT now(),
    FOREIGN KEY (run_id, game_id, owner_kind, owner_id)
        REFERENCES analysis_runs (id, game_id, owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS analysis_jobs (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    run_id text NOT NULL,
    game_id text NOT NULL,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    idempotency_key text NOT NULL CHECK (length(idempotency_key) BETWEEN 1 AND 256),
    priority integer NOT NULL DEFAULT 0,
    status text NOT NULL CHECK (status IN ('queued', 'running', 'completed', 'cancelled', 'failed')),
    attempt integer NOT NULL DEFAULT 0 CHECK (attempt >= 0),
    lease_owner text,
    lease_expires_at timestamptz,
    cancel_requested_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (id, run_id, owner_kind, owner_id),
    UNIQUE (run_id),
    UNIQUE (owner_kind, owner_id, idempotency_key),
    FOREIGN KEY (run_id, game_id, owner_kind, owner_id)
        REFERENCES analysis_runs (id, game_id, owner_kind, owner_id) ON DELETE CASCADE,
    CHECK ((lease_owner IS NULL) = (lease_expires_at IS NULL))
);

CREATE INDEX IF NOT EXISTS analysis_jobs_claim_index
    ON analysis_jobs (status, priority DESC, created_at)
    WHERE status = 'queued';

CREATE TABLE IF NOT EXISTS job_events (
    job_id text NOT NULL,
    run_id text NOT NULL,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    sequence integer NOT NULL CHECK (sequence >= 0),
    stage text NOT NULL CHECK (length(stage) BETWEEN 1 AND 64),
    completed_units integer NOT NULL CHECK (completed_units >= 0),
    total_units integer NOT NULL CHECK (total_units >= completed_units),
    created_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (job_id, sequence),
    FOREIGN KEY (job_id, run_id, owner_kind, owner_id)
        REFERENCES analysis_jobs (id, run_id, owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS chess_profiles (
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    provider text NOT NULL CHECK (provider IN ('chess.com')),
    username text NOT NULL CHECK (length(username) BETWEEN 1 AND 64),
    settings_json jsonb NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(settings_json) = 'object'),
    sync_cursor text,
    last_successful_sync_at timestamptz,
    last_error text,
    PRIMARY KEY (owner_kind, owner_id, provider),
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS variations (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    game_id text NOT NULL,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    root_ply integer NOT NULL CHECK (root_ply >= 0),
    root_fen text NOT NULL CHECK (length(root_fen) > 0),
    engine_configuration text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (id, game_id, owner_kind, owner_id),
    FOREIGN KEY (game_id, owner_kind, owner_id)
        REFERENCES game_owners (game_id, owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS variation_nodes (
    variation_id text NOT NULL,
    node_id bigint NOT NULL CHECK (node_id >= 0),
    parent_node_id bigint,
    uci text,
    san text,
    fen text NOT NULL CHECK (length(fen) > 0),
    PRIMARY KEY (variation_id, node_id),
    FOREIGN KEY (variation_id) REFERENCES variations (id) ON DELETE CASCADE,
    FOREIGN KEY (variation_id, parent_node_id)
        REFERENCES variation_nodes (variation_id, node_id),
    CHECK (parent_node_id IS NULL OR parent_node_id <> node_id)
);

CREATE TABLE IF NOT EXISTS review_attempts (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    game_id text NOT NULL,
    run_id text NOT NULL,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    ply integer NOT NULL CHECK (ply >= 0),
    uci text NOT NULL CHECK (length(uci) BETWEEN 4 AND 5),
    accepted boolean NOT NULL,
    attempted_at timestamptz NOT NULL DEFAULT now(),
    FOREIGN KEY (run_id, game_id, owner_kind, owner_id)
        REFERENCES analysis_runs (id, game_id, owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS intelligence_evidence (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    run_id text NOT NULL,
    game_id text NOT NULL,
    ply integer NOT NULL CHECK (ply >= 0),
    evidence_kind text NOT NULL CHECK (length(evidence_kind) BETWEEN 1 AND 128),
    model_version text NOT NULL CHECK (length(model_version) BETWEEN 1 AND 128),
    evidence_json jsonb NOT NULL CHECK (jsonb_typeof(evidence_json) = 'object'),
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (id, owner_kind, owner_id),
    FOREIGN KEY (run_id, game_id, owner_kind, owner_id)
        REFERENCES analysis_runs (id, game_id, owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS practice_items (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    evidence_id text NOT NULL,
    state text NOT NULL CHECK (state IN ('queued', 'active', 'completed', 'dismissed')),
    due_at timestamptz,
    schedule_version text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (id, owner_kind, owner_id),
    FOREIGN KEY (evidence_id, owner_kind, owner_id)
        REFERENCES intelligence_evidence (id, owner_kind, owner_id) ON DELETE CASCADE,
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS practice_outcomes (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    practice_item_id text NOT NULL,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    result text NOT NULL CHECK (result IN ('correct', 'incorrect', 'abandoned')),
    response_time_ms bigint NOT NULL CHECK (response_time_ms >= 0),
    hint_level integer NOT NULL CHECK (hint_level >= 0),
    attempted_at timestamptz NOT NULL DEFAULT now(),
    FOREIGN KEY (practice_item_id, owner_kind, owner_id)
        REFERENCES practice_items (id, owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS user_settings (
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    settings_version integer NOT NULL DEFAULT 1 CHECK (settings_version > 0),
    settings_json jsonb NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(settings_json) = 'object'),
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (owner_kind, owner_id),
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS idempotency_records (
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    operation text NOT NULL CHECK (length(operation) BETWEEN 1 AND 128),
    idempotency_key text NOT NULL CHECK (length(idempotency_key) BETWEEN 1 AND 256),
    request_hash bytea NOT NULL CHECK (octet_length(request_hash) = 32),
    resource_kind text,
    resource_id text,
    response_json jsonb,
    created_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL,
    PRIMARY KEY (owner_kind, owner_id, operation, idempotency_key),
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE,
    CHECK ((resource_kind IS NULL) = (resource_id IS NULL)),
    CHECK (response_json IS NULL OR jsonb_typeof(response_json) = 'object')
);

CREATE TABLE IF NOT EXISTS account_data_requests (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 256),
    owner_kind text NOT NULL CHECK (owner_kind = 'account'),
    owner_id text NOT NULL,
    request_kind text NOT NULL CHECK (request_kind IN ('export', 'delete')),
    idempotency_key text NOT NULL CHECK (length(idempotency_key) BETWEEN 1 AND 256),
    status text NOT NULL CHECK (status IN ('requested', 'running', 'completed', 'failed')),
    receipt_json jsonb,
    created_at timestamptz NOT NULL DEFAULT now(),
    completed_at timestamptz,
    UNIQUE (owner_kind, owner_id, request_kind, idempotency_key),
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES accounts (owner_kind, id) ON DELETE CASCADE,
    CHECK (receipt_json IS NULL OR jsonb_typeof(receipt_json) = 'object')
);

-- A deletion receipt intentionally has no owner foreign key so it can outlive removal of the
-- private account rows. The caller proves possession with the unhashed receipt token.
CREATE TABLE IF NOT EXISTS account_deletion_receipts (
    request_id text PRIMARY KEY CHECK (length(request_id) BETWEEN 1 AND 256),
    receipt_token_hash bytea NOT NULL UNIQUE CHECK (octet_length(receipt_token_hash) = 32),
    status text NOT NULL CHECK (status IN ('completed', 'failed')),
    completed_at timestamptz NOT NULL,
    expires_at timestamptz NOT NULL CHECK (expires_at > completed_at)
);

CREATE TABLE IF NOT EXISTS outbox_events (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    aggregate_kind text NOT NULL CHECK (length(aggregate_kind) BETWEEN 1 AND 128),
    aggregate_id text NOT NULL CHECK (length(aggregate_id) BETWEEN 1 AND 256),
    event_kind text NOT NULL CHECK (length(event_kind) BETWEEN 1 AND 128),
    payload_json jsonb NOT NULL CHECK (jsonb_typeof(payload_json) = 'object'),
    created_at timestamptz NOT NULL DEFAULT now(),
    published_at timestamptz,
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS outbox_events_unpublished_index
    ON outbox_events (id) WHERE published_at IS NULL;
