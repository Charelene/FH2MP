
    ALTER TABLE hidden_scores ADD COLUMN source TEXT NOT NULL DEFAULT 'operator';
    ALTER TABLE hidden_scores ADD COLUMN reason TEXT NOT NULL DEFAULT '';
    CREATE TABLE score_verdicts (
        submission_id BIGINT PRIMARY KEY REFERENCES score_submissions(id),
        version BIGINT NOT NULL,
        verdict TEXT NOT NULL,
        reasons TEXT NOT NULL,
        measurements TEXT NOT NULL,
        decided_at DOUBLE PRECISION NOT NULL,
        reviewed BIGINT NOT NULL DEFAULT 0
    );
    CREATE INDEX score_verdicts_open ON score_verdicts (verdict, reviewed);
    CREATE TABLE score_overrides (
        submission_id BIGINT PRIMARY KEY REFERENCES score_submissions(id),
        at DOUBLE PRECISION NOT NULL
    );
    