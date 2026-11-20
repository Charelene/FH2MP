    -- Avatars (native/internal/avatars, 2026-11-20): each account's Xbox 360 avatar as Encore's editor saves it, and the avatar items
    -- each account owns. Items are rewards (later The Eliminator's battle pass), with no store and no money.
    CREATE TABLE avatars (
        xuid       BIGINT           PRIMARY KEY,
        manifest   BYTEA            NOT NULL CHECK (octet_length(manifest) = 1000),  -- the avatar manifest (XAVATAR_METADATA) as sent
        colours    TEXT             NOT NULL DEFAULT '',  -- the editor's colour choices, one "<record> <palette>" per line
        body_png   BYTEA            NOT NULL,             -- the full-body picture Encore rendered, re-encoded by pngtool
        updated_at DOUBLE PRECISION NOT NULL              -- the card's avatarVersion: raised by every save
    );

    -- Owned items. item = the 16-byte asset id as the manifest holds it, in 32 lowercase hex digits; source = what granted it.
    CREATE TABLE avatar_items (
        xuid       BIGINT           NOT NULL,
        item       TEXT             NOT NULL CHECK (item ~ '^[0-9a-f]{32}$'),
        granted_at DOUBLE PRECISION NOT NULL,
        source     TEXT             NOT NULL,
        PRIMARY KEY (xuid, item)
    )
