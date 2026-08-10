CREATE TABLE IF NOT EXISTS chesscom_archive_entries (
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    game_id text NOT NULL CHECK (length(game_id) BETWEEN 1 AND 64),
    canonical_url text NOT NULL CHECK (length(canonical_url) BETWEEN 1 AND 2048),
    pgn text NOT NULL CHECK (length(pgn) > 0),
    username text NOT NULL CHECK (length(username) BETWEEN 1 AND 25),
    month text NOT NULL CHECK (month ~ '^[0-9]{4}-(0[1-9]|1[0-2])$'),
    time_class text NOT NULL CHECK (length(time_class) BETWEEN 1 AND 16),
    end_time_ms bigint NOT NULL CHECK (end_time_ms >= 0),
    fetched_at_ms bigint NOT NULL CHECK (fetched_at_ms >= 0),
    source_url text NOT NULL CHECK (length(source_url) BETWEEN 1 AND 2048),
    PRIMARY KEY (owner_kind, owner_id, game_id),
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS chesscom_archive_owner_date_index
    ON chesscom_archive_entries (owner_kind, owner_id, end_time_ms DESC, game_id);

CREATE TABLE IF NOT EXISTS chesscom_month_checkpoints (
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    username text NOT NULL CHECK (length(username) BETWEEN 1 AND 25),
    month text NOT NULL CHECK (month ~ '^[0-9]{4}-(0[1-9]|1[0-2])$'),
    source_url text NOT NULL CHECK (length(source_url) BETWEEN 1 AND 2048),
    indexed_games bigint NOT NULL CHECK (indexed_games >= 0),
    completed_at_ms bigint NOT NULL CHECK (completed_at_ms >= 0),
    PRIMARY KEY (owner_kind, owner_id, username, month),
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS chesscom_sync_states (
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    status text NOT NULL CHECK (status IN ('idle', 'running', 'paused', 'succeeded', 'failed')),
    username text NOT NULL DEFAULT '' CHECK (length(username) <= 25),
    cursor text NOT NULL DEFAULT '' CHECK (length(cursor) <= 512),
    current_month text NOT NULL DEFAULT '' CHECK (
        current_month = '' OR current_month ~ '^[0-9]{4}-(0[1-9]|1[0-2])$'
    ),
    months_completed bigint NOT NULL DEFAULT 0 CHECK (months_completed >= 0),
    games_indexed bigint NOT NULL DEFAULT 0 CHECK (games_indexed >= 0),
    started_at_ms bigint NOT NULL DEFAULT 0 CHECK (started_at_ms >= 0),
    updated_at_ms bigint NOT NULL DEFAULT 0 CHECK (updated_at_ms >= 0),
    last_error text NOT NULL DEFAULT '',
    PRIMARY KEY (owner_kind, owner_id),
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE
);
