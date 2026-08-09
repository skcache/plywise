-- Keep deletion replay safe without retaining a bearer token or a live owner foreign key.
ALTER TABLE account_deletion_receipts
    ADD COLUMN IF NOT EXISTS account_id text,
    ADD COLUMN IF NOT EXISTS auth_provider text,
    ADD COLUMN IF NOT EXISTS auth_subject_hash bytea,
    ADD COLUMN IF NOT EXISTS idempotency_key text;

ALTER TABLE account_deletion_receipts
    DROP CONSTRAINT IF EXISTS account_deletion_receipts_auth_subject_hash_check;
ALTER TABLE account_deletion_receipts
    ADD CONSTRAINT account_deletion_receipts_auth_subject_hash_check
    CHECK (auth_subject_hash IS NULL OR octet_length(auth_subject_hash) = 32);

CREATE UNIQUE INDEX IF NOT EXISTS account_deletion_receipts_identity_key
    ON account_deletion_receipts (auth_provider, auth_subject_hash, idempotency_key)
    WHERE auth_provider IS NOT NULL AND auth_subject_hash IS NOT NULL AND idempotency_key IS NOT NULL;
