    -- MTProto chat P3, end-to-end direct messages (native/internal/mt e2e_api.go). The server keeps who talks to whom on which device,
    -- the public halves of each secret chat's key exchange with their device signatures, and the sealed bytes until the recipient has them;
    -- never a chat key, a secret exponent, a plaintext or a device's private key.

    -- One current chat device per account: its Ed25519 key, signed by the account key over "FH2MP-CHAT-DEVICE-v1\0" realm "\0" device_id pubkey.
    CREATE TABLE mt_chat_devices (
        xuid        BIGINT  PRIMARY KEY,
        device_id   BIGINT  NOT NULL,
        pubkey      BYTEA   NOT NULL,
        account_key BYTEA   NOT NULL,
        signature   BYTEA   NOT NULL,
        date        INTEGER NOT NULL
    );

    -- Secret chats. state 0 requested, 1 accepted, 2 discarded; changed = when it last changed state (discarded ones go after 30 days).
    CREATE TABLE mt_e2e_chats (
        id                 SERIAL           PRIMARY KEY,
        date               INTEGER          NOT NULL,
        admin              BIGINT           NOT NULL,
        admin_device       BIGINT           NOT NULL,
        participant        BIGINT           NOT NULL,
        participant_device BIGINT           NOT NULL,
        random_id          INTEGER          NOT NULL,
        state              SMALLINT         NOT NULL,
        g_a                BYTEA            NOT NULL,
        g_a_sig            BYTEA            NOT NULL,
        g_b                BYTEA,
        g_b_sig            BYTEA,
        key_fingerprint    BIGINT,
        changed            DOUBLE PRECISION NOT NULL
    );
    CREATE INDEX mt_e2e_chats_admin ON mt_e2e_chats (admin, random_id);
    CREATE INDEX mt_e2e_chats_participant ON mt_e2e_chats (participant);
    CREATE INDEX mt_e2e_chats_changed ON mt_e2e_chats (changed) WHERE state = 2;

    -- Each relayed message's franking commitment, kept 30 days for reports (no ciphertext); the key also answers a repeated random_id.
    CREATE TABLE mt_e2e_sent (
        chat       INTEGER          NOT NULL,
        sender     BIGINT           NOT NULL,
        random_id  BIGINT           NOT NULL,
        recipient  BIGINT           NOT NULL,
        commitment BYTEA            NOT NULL,
        date       INTEGER          NOT NULL,
        created    DOUBLE PRECISION NOT NULL,
        PRIMARY KEY (chat, sender, random_id)
    );
    CREATE INDEX mt_e2e_sent_created ON mt_e2e_sent (created);

    -- Reports whose text and franking key matched the stored commitment: the operators' list. Nothing acts on it automatically.
    CREATE TABLE mt_e2e_reports (
        id        BIGSERIAL        PRIMARY KEY,
        created   DOUBLE PRECISION NOT NULL,
        reporter  BIGINT           NOT NULL,
        sender    BIGINT           NOT NULL,
        chat      INTEGER          NOT NULL,
        random_id BIGINT           NOT NULL,
        text      TEXT             NOT NULL,
        reason    TEXT             NOT NULL,
        UNIQUE (reporter, chat, random_id)
    );

    -- The stored updates of secret chats (updateEncryption, updateNewEncryptedMessage) name their chat: they stay past an acked push until
    -- messages.ackEncrypted covers them, a discard drops the chat's sealed messages, or 7 days. gone = the highest pts of a row the store
    -- dropped that the client may not have had (asking from below it is answered differenceTooLong).
    ALTER TABLE mt_updates ADD COLUMN e2e_chat INTEGER;
    ALTER TABLE mt_updates ADD COLUMN e2e_message BOOLEAN NOT NULL DEFAULT FALSE;
    CREATE INDEX mt_updates_e2e_chat ON mt_updates (e2e_chat) WHERE e2e_chat IS NOT NULL;
    ALTER TABLE mt_state ADD COLUMN gone INTEGER NOT NULL DEFAULT 0
