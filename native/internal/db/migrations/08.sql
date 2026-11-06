
    ALTER TABLE score_submissions ADD COLUMN autobrake BIGINT NOT NULL DEFAULT 0;
    ALTER TABLE score_submissions ADD COLUMN autoshift BIGINT NOT NULL DEFAULT 0;
    ALTER TABLE score_submissions ADD COLUMN clutch BIGINT NOT NULL DEFAULT 0
    