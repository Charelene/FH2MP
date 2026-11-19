    ALTER TABLE player_profiles ADD COLUMN motto     TEXT NOT NULL DEFAULT '';  -- the Guide's profile texts (2026-11-11)
    ALTER TABLE player_profiles ADD COLUMN real_name TEXT NOT NULL DEFAULT '';
    ALTER TABLE player_profiles ADD COLUMN location  TEXT NOT NULL DEFAULT '';
    ALTER TABLE player_profiles ADD COLUMN bio       TEXT NOT NULL DEFAULT '';
