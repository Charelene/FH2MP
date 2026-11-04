
CREATE TABLE accounts (
    xuid         BIGINT PRIMARY KEY,
    pubkey       BYTEA  NOT NULL UNIQUE,              -- Ed25519, 32 bytes
    gamertag     TEXT   NOT NULL,
    gamertag_key TEXT   NOT NULL UNIQUE,              -- folded form, for uniqueness
    created      DOUBLE PRECISION NOT NULL,
    created_ip   TEXT   NOT NULL DEFAULT '',
    last_login   DOUBLE PRECISION NOT NULL DEFAULT 0,
    tag_changed  DOUBLE PRECISION NOT NULL DEFAULT 0,
    banned_at    DOUBLE PRECISION,
    ban_reason   TEXT   NOT NULL DEFAULT ''
);
CREATE INDEX accounts_created ON accounts (created);

CREATE TABLE auth_sessions (
    token_hash   BYTEA PRIMARY KEY,                   -- SHA-256 of the token; the token itself is never stored
    xuid         BIGINT NOT NULL REFERENCES accounts (xuid) ON DELETE CASCADE,
    ip           TEXT   NOT NULL,
    client       TEXT   NOT NULL DEFAULT '',
    created      DOUBLE PRECISION NOT NULL,
    last_used    DOUBLE PRECISION NOT NULL,
    expires      DOUBLE PRECISION NOT NULL,
    hard_expires DOUBLE PRECISION NOT NULL,
    revoked      BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX auth_sessions_xuid ON auth_sessions (xuid);

CREATE TABLE auth_nonces (
    nonce   BYTEA PRIMARY KEY,
    expires DOUBLE PRECISION NOT NULL
);

CREATE TABLE auth_events (
    id     BIGSERIAL PRIMARY KEY,
    at     DOUBLE PRECISION NOT NULL,
    kind   TEXT NOT NULL,
    ip     TEXT NOT NULL DEFAULT '',
    xuid   BIGINT,
    detail TEXT NOT NULL DEFAULT ''
);
CREATE INDEX auth_events_at ON auth_events (at);

CREATE TABLE invites (
    code_hash BYTEA PRIMARY KEY,
    uses_left BIGINT NOT NULL DEFAULT 1,
    note      TEXT   NOT NULL DEFAULT '',
    created   DOUBLE PRECISION NOT NULL
)
    