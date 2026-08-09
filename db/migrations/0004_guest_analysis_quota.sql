CREATE TABLE IF NOT EXISTS guest_analysis_reservations (
    guest_id text PRIMARY KEY CHECK (length(guest_id) BETWEEN 1 AND 256),
    owner_kind text NOT NULL DEFAULT 'guest' CHECK (owner_kind = 'guest'),
    game_id text NOT NULL CHECK (length(game_id) BETWEEN 1 AND 256),
    reserved_at timestamptz NOT NULL DEFAULT now(),
    FOREIGN KEY (game_id, owner_kind, guest_id)
        REFERENCES game_owners (game_id, owner_kind, owner_id) ON DELETE CASCADE
);
