ALTER TABLE plywise.variations
    ADD COLUMN IF NOT EXISTS root_position text NOT NULL DEFAULT 'after'
        CHECK (root_position IN ('before', 'after'));

ALTER TABLE plywise.variations
    ADD COLUMN IF NOT EXISTS current_node_id bigint NOT NULL DEFAULT 0
        CHECK (current_node_id >= 0);
