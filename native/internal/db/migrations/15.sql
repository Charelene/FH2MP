
    CREATE TABLE ugc_actions (
        file_id BIGINT NOT NULL,
        xuid    BIGINT NOT NULL,
        action  TEXT   NOT NULL,                  -- download | like
        at      DOUBLE PRECISION NOT NULL,
        PRIMARY KEY (file_id, xuid, action)
    )
    