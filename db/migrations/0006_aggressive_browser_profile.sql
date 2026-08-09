ALTER TABLE browser_observation_runs
    DROP CONSTRAINT IF EXISTS browser_observation_runs_profile_check;

ALTER TABLE browser_observation_runs
    ADD CONSTRAINT browser_observation_runs_profile_check
    CHECK (profile IN ('quick', 'balanced', 'aggressive'));
