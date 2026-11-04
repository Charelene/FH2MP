
    UPDATE club_members SET role = role + 1;
    UPDATE club_history SET role = role + 1 WHERE role < 3;
    UPDATE club_notices SET role = role + 1;
    ALTER TABLE club_members ALTER COLUMN role SET DEFAULT 1;
    ALTER TABLE club_members ADD COLUMN xp_base DOUBLE PRECISION NOT NULL DEFAULT 0;
    UPDATE club_members m SET xp_base = COALESCE((SELECT xp FROM player_xp x WHERE x.xuid = m.xuid), 0);
    