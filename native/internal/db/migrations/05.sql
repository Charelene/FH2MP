
    CREATE TABLE friendships (
        a       BIGINT NOT NULL,                   -- always the smaller xuid
        b       BIGINT NOT NULL,
        created DOUBLE PRECISION NOT NULL,
        PRIMARY KEY (a, b),
        CHECK (a < b)
    );
    CREATE INDEX friendships_b ON friendships (b);

    CREATE TABLE player_profiles (
        xuid          BIGINT PRIMARY KEY,
        display_name  TEXT   NOT NULL DEFAULT '',  -- empty = the gamertag
        gamerscore    BIGINT NOT NULL DEFAULT 0,
        tier          TEXT   NOT NULL DEFAULT 'Gold',
        gamerpic      BYTEA,                       -- PNG as uploaded (LZMA-packed, ugc.pack); NULL = a generated one
        gamerpic_at   DOUBLE PRECISION
    );
    