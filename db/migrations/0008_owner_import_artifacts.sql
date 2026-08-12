-- Exact imported PGN and source provenance belong to the importing owner.
-- Existing rows remain untouched; the adapter supplies an annotation-free
-- canonical fallback until an owner imports that game again.

ALTER TABLE game_owners
    ADD COLUMN imported_pgn text,
    ADD COLUMN source_url text;

ALTER TABLE game_owners
    ADD CONSTRAINT game_owners_imported_pgn_nonempty
        CHECK (imported_pgn IS NULL OR
               (octet_length(imported_pgn) BETWEEN 1 AND 10485760)),
    ADD CONSTRAINT game_owners_source_url_bounded
        CHECK (source_url IS NULL OR length(source_url) <= 2048);
