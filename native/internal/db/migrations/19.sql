    -- MTProto chat (native/internal/mt): each account's stored update sequence. pts counts up from 1 per account; a row goes once the
    -- client has it (an acked push, or updates.getDifference from a later pts) or after 7 days.
    CREATE TABLE mt_state (
        xuid BIGINT  PRIMARY KEY,
        pts  INTEGER NOT NULL
    );
    CREATE TABLE mt_updates (
        xuid    BIGINT           NOT NULL,
        pts     INTEGER          NOT NULL,
        created DOUBLE PRECISION NOT NULL,
        body    BYTEA            NOT NULL,   -- one TL-encoded Update of proto/fh2mp.tl
        PRIMARY KEY (xuid, pts)
    );
    CREATE INDEX mt_updates_created ON mt_updates (created)
