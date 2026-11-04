
    CREATE TABLE score_reports (
        submission_id BIGINT NOT NULL REFERENCES score_submissions(id),
        browser TEXT NOT NULL,
        ip TEXT NOT NULL,
        created DOUBLE PRECISION NOT NULL,
        dismissed BIGINT NOT NULL DEFAULT 0,
        PRIMARY KEY (submission_id, browser),
        UNIQUE (submission_id, ip)
    );
    CREATE INDEX score_reports_browser ON score_reports (browser, created);
    CREATE INDEX score_reports_ip ON score_reports (ip, created);
    CREATE TABLE hidden_scores (
        submission_id BIGINT PRIMARY KEY REFERENCES score_submissions(id),
        hidden_at DOUBLE PRECISION NOT NULL
    )
    