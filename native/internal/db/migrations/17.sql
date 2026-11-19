    CREATE TABLE player_achievements (            -- what a player's Guide reported as unlocked (the Achievements mod decides them; 2026-11-11)
        xuid        BIGINT NOT NULL,
        achievement TEXT   NOT NULL,              -- the id in internal/achievements
        unlocked_at DOUBLE PRECISION NOT NULL,
        PRIMARY KEY (xuid, achievement)
    )
