-- Exact imported PGN and source provenance belong to the importing owner.
-- Existing raw PGN attribution is unknowable, so legacy imported_pgn stays
-- NULL and the adapter supplies an annotation-free canonical fallback. A
-- legacy public URL can be recovered safely from that owner's own source key.

ALTER TABLE game_owners
    ADD COLUMN imported_pgn text,
    ADD COLUMN source_url text;

ALTER TABLE game_owners
    ADD CONSTRAINT game_owners_imported_pgn_nonempty
        CHECK (imported_pgn IS NULL OR
               (octet_length(imported_pgn) BETWEEN 1 AND 10485760)),
    ADD CONSTRAINT game_owners_source_url_bounded
        CHECK (source_url IS NULL OR octet_length(source_url) <= 2048);

UPDATE game_owners
SET source_url = source_key
WHERE source_key IS NOT NULL
  AND source_key LIKE 'https://%'
  AND octet_length(source_key) <= 2048;

-- Export receipts are derived caches. Regenerate old receipts through the
-- owner-local query instead of replaying a pre-migration shared PGN payload.
UPDATE account_data_requests
SET status = 'requested', receipt_json = NULL, completed_at = NULL
WHERE request_kind = 'export' AND receipt_json IS NOT NULL;
