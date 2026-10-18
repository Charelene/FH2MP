"""SQLite storage for the LSP stand-in (scores, XP, Drivatars, notifications, players).

One file, `server/data/fh2mp.sqlite` (override with FH2MP_DATA_DIR / FH2MP_DB), stdlib only. SQLite in WAL mode
gives ACID writes and row-level updates instead of rewriting whole JSON files, and lets `sqlite3` inspect the file while
the server runs. Only durable state lives here: sessions, tickets and lobbies (xbl.py) and every
live game world (fh2mp_server.py) stay in process memory on purpose, they are leases that should vanish on restart.

One shared connection guarded by a lock (ThreadingHTTPServer handles each request on its own thread). Writes go through
`tx()`, which opens the transaction with BEGIN IMMEDIATE so a read-modify-write cannot interleave with another writer.
Schema changes are numbered entries in MIGRATIONS, applied in order and recorded in PRAGMA user_version.
SQL sticks to what PostgreSQL also has (upserts, RETURNING, window functions), so a later move to PostgreSQL is mostly
this module plus the few SQLite-isms (AUTOINCREMENT, json_each, sqlite_sequence).
"""
import json
import os
import sqlite3
import threading
import time
from contextlib import contextmanager

MIGRATIONS = [
    # 1: initial schema
    """
    CREATE TABLE players (
        xuid          INTEGER PRIMARY KEY,
        gamertag      TEXT    NOT NULL DEFAULT '',
        career_level  INTEGER NOT NULL DEFAULT 0,
        first_seen    REAL    NOT NULL,
        last_seen     REAL    NOT NULL
    );

    -- One row per accepted score. Raw protocol ids are kept as sent; which direction is "better" is decided
    -- at query time (scoreboard.ASCENDING is still a guess), so nothing here bakes it in.
    CREATE TABLE score_submissions (
        id         INTEGER PRIMARY KEY,
        xuid       INTEGER NOT NULL,
        sb         INTEGER NOT NULL,      -- scoreboardType
        st         INTEGER NOT NULL,      -- scoreType
        gsid       INTEGER NOT NULL,      -- gameScoreboardId
        track      INTEGER NOT NULL,
        car        INTEGER NOT NULL,
        cls        INTEGER NOT NULL,
        cpi        REAL    NOT NULL,
        score      REAL    NOT NULL,      -- decoded IEEE double (finite only, see ScoreStore.submit)
        clean      INTEGER NOT NULL,
        stm        INTEGER NOT NULL,
        tcs        INTEGER NOT NULL,
        abs        INTEGER NOT NULL,
        submitted  REAL    NOT NULL
    );
    CREATE INDEX score_board ON score_submissions (sb, st, track, xuid);
    CREATE INDEX score_player ON score_submissions (xuid, sb, st, track);

    CREATE TABLE player_xp (
        xuid     INTEGER PRIMARY KEY,
        xp       REAL    NOT NULL,
        updated  REAL    NOT NULL
    );

    -- A player's own Drivatar. The wire-shaped record (UserInfo, Curves, Segments, ...) stays opaque JSON;
    -- the columns are the fields we select on.
    CREATE TABLE drivatars (
        xuid       INTEGER PRIMARY KEY,
        gamertag   TEXT    NOT NULL DEFAULT '',
        skill      INTEGER NOT NULL,
        wristband  INTEGER NOT NULL,
        locked     INTEGER NOT NULL DEFAULT 0,
        record     TEXT    NOT NULL,
        updated    REAL    NOT NULL
    );
    CREATE INDEX drivatar_skill ON drivatars (skill, wristband);

    -- Immutable content; per-player state lives in notification_receipts so one player reading, clearing or
    -- claiming a broadcast does not change it for everyone. xuid 0 = broadcast. AUTOINCREMENT: ids are never
    -- reused, the client keys its Message Center on them.
    CREATE TABLE notifications (
        id       INTEGER PRIMARY KEY AUTOINCREMENT,
        type     INTEGER NOT NULL,
        message  TEXT    NOT NULL DEFAULT '',
        sra      TEXT    NOT NULL DEFAULT '',
        sent     INTEGER NOT NULL,        -- Windows FILETIME, as the client wants it
        xuid     INTEGER NOT NULL DEFAULT 0,
        car_id   INTEGER,
        params   TEXT    NOT NULL DEFAULT '{}'
    );
    CREATE INDEX notification_xuid ON notifications (xuid);

    CREATE TABLE notification_receipts (
        notification_id INTEGER NOT NULL REFERENCES notifications (id) ON DELETE CASCADE,
        xuid            INTEGER NOT NULL,
        read_at         REAL,
        cleared_at      REAL,
        claimed_at      REAL,
        PRIMARY KEY (notification_id, xuid)
    );

    CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
    """,
    # 2: ghosts. SubmitScore hands the client a random scoreId; the client then uploads the ghost (a replay blob) for it
    # (Scoreboard.UploadFile). The row's ghostFileId is the submission id; DownloadFile serves the blob back to whoever races it.
    """
    ALTER TABLE score_submissions ADD COLUMN score_id BLOB;
    CREATE INDEX score_by_scoreid ON score_submissions (score_id);
    CREATE TABLE score_ghosts (
        submission_id INTEGER PRIMARY KEY REFERENCES score_submissions (id) ON DELETE CASCADE,
        data          BLOB NOT NULL,
        uploaded      REAL NOT NULL
    );
    """,
    # 3: Monthly Rivals events. A rotation is generated once (server/rivals.py), stored here and never regenerated, so an event id
    # keeps its meaning (track, laps, restriction, text) after restarts and code changes; scores, ghosts and rival notifications
    # reference these ids. Ended rotations stay for history.
    """
    CREATE TABLE rival_events (
        id          INTEGER PRIMARY KEY,           -- 16-bit on the wire (ForzaTimeTrial.id)
        month       INTEGER NOT NULL,              -- months since 2026-01
        reroll      INTEGER NOT NULL,
        idx         INTEGER NOT NULL,
        track       INTEGER NOT NULL,
        laps        INTEGER NOT NULL,
        length      REAL    NOT NULL,              -- metres, from the route file
        kind        TEXT    NOT NULL,
        attrs       TEXT    NOT NULL,              -- JSON: CarRestrictions attributes
        cls         INTEGER,                       -- class used for the rival time model
        name        TEXT    NOT NULL,
        trackname   TEXT    NOT NULL,
        description TEXT    NOT NULL,
        created     REAL    NOT NULL
    );
    CREATE INDEX rival_events_rotation ON rival_events (month, reroll, idx);
    """,
    # 4: time of day and weather per rival event. tod = the GameOptions TimeOfDay value (NULL = the world's current time; unit under test:
    # the career data stores HHMM), weather = AtmosphericPresetIndex (WeatherPreset.Id) with the wetness / precipitation levels.
    """
    ALTER TABLE rival_events ADD COLUMN tod INTEGER;
    ALTER TABLE rival_events ADD COLUMN weather INTEGER;
    ALTER TABLE rival_events ADD COLUMN wetness REAL;
    ALTER TABLE rival_events ADD COLUMN precip REAL;
    """,
]


class Database:
    def __init__(self, path):
        self.path = path
        if path != ":memory:":
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        # One shared connection behind an RLock: ThreadingHTTPServer starts a thread per request, so per-thread
        # connections would pile up, and at this scale (a dozen players) serialising statements costs nothing.
        self._lock = threading.RLock()
        self.conn = sqlite3.connect(path, timeout=10, isolation_level=None, check_same_thread=False, uri=True)  # autocommit; tx() does BEGIN
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA foreign_keys=ON")
        self.conn.execute("PRAGMA busy_timeout=10000")
        if path != ":memory:":
            self.conn.execute("PRAGMA journal_mode=WAL")
            self.conn.execute("PRAGMA synchronous=NORMAL")   # WAL: a power cut can lose the last commits, never corrupt
        self._migrate()

    @contextmanager
    def tx(self):
        """A write transaction: BEGIN IMMEDIATE .. COMMIT, rolled back if the body raises. Holds the lock throughout."""
        with self._lock:
            c = self.conn
            c.execute("BEGIN IMMEDIATE")
            try:
                yield c
            except BaseException:
                c.execute("ROLLBACK")
                raise
            c.execute("COMMIT")

    def query(self, sql, args=()):
        with self._lock:
            return self.conn.execute(sql, args).fetchall()

    def one(self, sql, args=()):
        with self._lock:
            return self.conn.execute(sql, args).fetchone()

    def attach(self, alias, path, readonly=True):
        """Attaches another SQLite file under `alias` (the game catalog is attached as `game`, server/catalog.py) so state and
        game data can be joined in one statement. Read-only by default."""
        with self._lock:
            uri = f"file:{path}?mode={'ro' if readonly else 'rw'}"
            self.conn.execute(f"ATTACH DATABASE '{uri}' AS \"{alias}\"")

    def close(self):
        with self._lock:
            self.conn.close()

    # ---- schema
    def _migrate(self):
        c = self.conn
        version = c.execute("PRAGMA user_version").fetchone()[0]
        if version > len(MIGRATIONS):
            raise RuntimeError(f"{self.path} is schema v{version}, this build only knows v{len(MIGRATIONS)}")
        for v in range(version, len(MIGRATIONS)):
            c.execute("BEGIN IMMEDIATE")
            try:
                for stmt in _statements(MIGRATIONS[v]):
                    c.execute(stmt)
                c.execute(f"PRAGMA user_version={v + 1}")      # transactional in SQLite
            except BaseException:
                c.execute("ROLLBACK")
                raise
            c.execute("COMMIT")

    # ---- meta
    def meta_get(self, key, default=None):
        r = self.one("SELECT value FROM meta WHERE key=?", (key,))
        return r["value"] if r else default


def touch_player(c, xuid, tag="", seen=None, level=None):
    """Creates the player row or refreshes last_seen (and the gamertag / career level when given).
    `c` is a tx() connection."""
    seen = time.time() if seen is None else seen
    c.execute("INSERT INTO players (xuid, gamertag, career_level, first_seen, last_seen) VALUES (?,?,?,?,?) "
              "ON CONFLICT(xuid) DO UPDATE SET last_seen=excluded.last_seen, "
              "gamertag=CASE WHEN excluded.gamertag<>'' THEN excluded.gamertag ELSE gamertag END, "
              "career_level=CASE WHEN ? IS NULL THEN career_level ELSE excluded.career_level END",
              (xuid, tag or "", level or 0, seen, seen, level))


def _statements(script):
    """Split a migration script into statements: drop `--` comment lines (they may contain ';'), then split on ';'
    (no trigger bodies or string literals with ';' in ours, so a plain split is enough)."""
    text = "\n".join(ln for ln in script.splitlines() if not ln.strip().startswith("--"))
    return [part.strip() for part in text.split(";") if part.strip()]


def open_db(data_dir):
    """Opens (creating if needed) the database in `data_dir` and imports the legacy JSON files once."""
    path = os.environ.get("FH2MP_DB") or os.path.join(data_dir, "fh2mp.sqlite")
    db = Database(path)
    import_legacy_json(db, data_dir)
    return db


# ---------------------------------------------------------------- one-time import of the old JSON stores

def _set_sequence(c, table, seq):
    """AUTOINCREMENT counter: the next id handed out is seq + 1."""
    if c.execute("UPDATE sqlite_sequence SET seq=? WHERE name=?", (seq, table)).rowcount == 0:
        c.execute("INSERT INTO sqlite_sequence (name, seq) VALUES (?,?)", (table, seq))


def import_legacy_json(db, data_dir):
    """scores.json / drivatars.json / notifications.json -> tables, in one transaction. Runs once (flag in `meta`);
    the JSON files are renamed to *.json.migrated afterwards, never deleted."""
    if db.meta_get("legacy_json_imported"):
        return
    paths = {n: os.path.join(data_dir, n + ".json") for n in ("scores", "drivatars", "notifications")}
    found = {n: p for n, p in paths.items() if os.path.exists(p)}
    now = time.time()
    counts = {}
    with db.tx() as c:
        player = lambda xuid, tag="", seen=now: touch_player(c, xuid, tag, seen)

        if "scores" in found:
            with open(found["scores"]) as f:
                d = json.load(f)
            subs = d.get("subs", [])
            for s in subs:
                player(s["xuid"], s.get("tag", ""), s.get("t", now))
                c.execute("INSERT INTO score_submissions (xuid, sb, st, gsid, track, car, cls, cpi, score, clean, stm, "
                          "tcs, abs, submitted) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                          (s["xuid"], s["sb"], s["st"], s["gsid"], s["track"], s["car"], s["cls"], s["cpi"],
                           s["score"], int(s["clean"]), int(s["stm"]), int(s["tcs"]), int(s["abs"]), s["t"]))
            for xuid, xp in d.get("xp", {}).items():
                player(int(xuid))
                c.execute("INSERT INTO player_xp (xuid, xp, updated) VALUES (?,?,?)", (int(xuid), xp, now))
            counts["scores"] = (len(subs), len(d.get("xp", {})))

        if "drivatars" in found:
            with open(found["drivatars"]) as f:
                players = json.load(f).get("players", {})
            for rec in players.values():
                locked = bool(rec.pop("_locked", False))
                player(rec["Xuid"], rec.get("GamerTag", ""))
                c.execute("INSERT INTO drivatars (xuid, gamertag, skill, wristband, locked, record, updated) "
                          "VALUES (?,?,?,?,?,?,?)", (rec["Xuid"], rec.get("GamerTag", ""), rec["UserSkillLevel"],
                                                     rec["wristbandLevel"], int(locked), json.dumps(rec), now))
            counts["drivatars"] = len(players)

        if "notifications" in found:
            with open(found["notifications"]) as f:
                d = json.load(f)
            for n in d.get("items", []):
                params = n.get("params", {})
                c.execute("INSERT INTO notifications (id, type, message, sra, sent, xuid, car_id, params) "
                          "VALUES (?,?,?,?,?,?,?,?)", (n["id"], n["type"], n.get("message", ""), n.get("sra", ""),
                                                       n["sent"], n.get("xuid", 0), n.get("carId"), json.dumps(params)))
                if n.get("read") or n.get("claimed"):
                    # the old flags were global. Addressed ones belong to that player; a read/claimed
                    # broadcast counts as read/claimed for every player we know of
                    who = [n["xuid"]] if n.get("xuid") else [r[0] for r in c.execute("SELECT xuid FROM players")]
                    for x in who:
                        c.execute("INSERT INTO notification_receipts (notification_id, xuid, read_at, claimed_at) "
                                  "VALUES (?,?,?,?)", (n["id"], x, now if n.get("read") else None,
                                                       now if n.get("claimed") else None))
            counts["notifications"] = len(d.get("items", []))
            # ids must keep growing past anything the client has ever seen, including the old counter
            top = c.execute("SELECT COALESCE(MAX(id), 0) FROM notifications").fetchone()[0]
            _set_sequence(c, "notifications", max(int(d.get("next_id", 0)) - 1, top, int(now) - 1))
        else:
            _set_sequence(c, "notifications", int(now) - 1)
        c.execute("INSERT INTO meta (key, value) VALUES ('legacy_json_imported', ?)", (json.dumps(counts),))
    for p in found.values():
        os.replace(p, p + ".migrated")
    if found:
        print(f"{time.strftime('%H:%M:%S')} imported legacy JSON into {db.path}: {counts}", flush=True)
