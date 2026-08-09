CREATE TABLE IF NOT EXISTS browser_observation_runs (
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    game_id text NOT NULL,
    analysis_run_id text NOT NULL CHECK (length(analysis_run_id) BETWEEN 1 AND 128),
    profile text NOT NULL CHECK (profile IN ('quick', 'balanced')),
    expected_observations integer NOT NULL CHECK (expected_observations BETWEEN 0 AND 4097),
    next_sequence integer NOT NULL DEFAULT 0 CHECK (next_sequence BETWEEN 0 AND 4097),
    finalized boolean NOT NULL DEFAULT false,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (owner_kind, owner_id, analysis_run_id),
    FOREIGN KEY (owner_kind, owner_id)
        REFERENCES owners (owner_kind, owner_id) ON DELETE CASCADE,
    FOREIGN KEY (game_id, owner_kind, owner_id)
        REFERENCES game_owners (game_id, owner_kind, owner_id) ON DELETE CASCADE,
    CHECK ((expected_observations = 0 AND next_sequence = 0) OR
           next_sequence <= expected_observations)
);

CREATE TABLE IF NOT EXISTS browser_observations (
    owner_kind text NOT NULL,
    owner_id text NOT NULL,
    analysis_run_id text NOT NULL,
    sequence integer NOT NULL CHECK (sequence BETWEEN 0 AND 4096),
    ply integer NOT NULL CHECK (ply BETWEEN 0 AND 20000),
    observation_json jsonb NOT NULL CHECK (jsonb_typeof(observation_json) = 'object'),
    created_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (owner_kind, owner_id, analysis_run_id, sequence),
    FOREIGN KEY (owner_kind, owner_id, analysis_run_id)
        REFERENCES browser_observation_runs (owner_kind, owner_id, analysis_run_id)
        ON DELETE CASCADE
);
