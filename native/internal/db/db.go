// Package db is the PostgreSQL storage of the LSP (= server/db.py): a small pool, write transactions serialised by an advisory lock, the numbered
// migrations, and the SQLite-style statement spelling (`?`, INSERT OR IGNORE) the stores are written in, translated here once.
package db

import (
	"context"
	"embed"
	"errors"
	"fmt"
	"math"
	"math/big"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgtype"
	"github.com/jackc/pgx/v5/pgxpool"

	"fh2mp/internal/diag"
)

//go:embed migrations/*.sql
var migrationFS embed.FS

var (
	logger = diag.Get("db")
	rate   = diag.NewRateLimit(nil, 0)
)

// Limits (db.py).
var (
	SlowQuerySeconds = envFloat("FH2MP_DB_SLOW_S", 1.0)
)

const (
	SlowLockSeconds = 1.0
	SlowTxSeconds   = 2.0
	DBLogSeconds    = 60.0
	AdvisoryLock    = 7240001
)

func envFloat(name string, def float64) float64 {
	if v, err := strconv.ParseFloat(os.Getenv(name), 64); err == nil {
		return v
	}
	return def
}

func sqlHead(sql string, n int) string {
	s := strings.TrimSpace(regexp.MustCompile(`\s+`).ReplaceAllString(sql, " "))
	if len(s) > n {
		s = s[:n]
	}
	return s
}

// Row is a result row that answers by column name (case-insensitive) or position.
type Row struct {
	names []string
	idx   map[string]int
	Vals  []any
}

// Keys lists the column names.
func (r *Row) Keys() []string { return r.names }

// Has reports whether the column exists.
func (r *Row) Has(name string) bool { _, ok := r.idx[strings.ToLower(name)]; return ok }

// Any returns a column value.
func (r *Row) Any(name string) any {
	i, ok := r.idx[strings.ToLower(name)]
	if !ok {
		panic("db: no column " + name)
	}
	return r.Vals[i]
}

// At returns the i-th column.
func (r *Row) At(i int) any { return r.Vals[i] }

// Int is an integer column (NULL = 0).
func (r *Row) Int(name string) int64 { return toInt(r.Any(name)) }

// IntAt is the i-th column as an integer.
func (r *Row) IntAt(i int) int64 { return toInt(r.Vals[i]) }

// Float is a float column (NULL = 0).
func (r *Row) Float(name string) float64 { return toFloat(r.Any(name)) }

// FloatAt is the i-th column as a float.
func (r *Row) FloatAt(i int) float64 { return toFloat(r.Vals[i]) }

// Str is a text column (NULL = "").
func (r *Row) Str(name string) string { return toStr(r.Any(name)) }

// StrAt is the i-th column as text.
func (r *Row) StrAt(i int) string { return toStr(r.Vals[i]) }

// Bytes is a bytea column.
func (r *Row) Bytes(name string) []byte { b, _ := r.Any(name).([]byte); return b }

// IsNull reports a NULL column.
func (r *Row) IsNull(name string) bool { return r.Any(name) == nil }

func toInt(v any) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case int32:
		return int64(x)
	case int16:
		return int64(x)
	case int:
		return int64(x)
	case float64:
		return int64(x)
	case bool:
		if x {
			return 1
		}
	}
	return 0
}

func toFloat(v any) float64 {
	switch x := v.(type) {
	case float64:
		return x
	case float32:
		return float64(x)
	case int64:
		return float64(x)
	case int32:
		return float64(x)
	}
	return 0
}

func toStr(v any) string {
	switch x := v.(type) {
	case string:
		return x
	case []byte:
		return string(x)
	}
	return ""
}

// Result is what Exec returns: rows affected and, for an INSERT into an identity table, the new id.
type Result struct {
	RowsAffected int64
	LastID       int64
	HasLastID    bool
}

var insertRe = regexp.MustCompile(`(?i)^\s*INSERT\s+(OR\s+IGNORE\s+)?INTO\s+(\w+)`)
var sqliteOnly = regexp.MustCompile(`(?i)\bINSERT\s+OR\s+REPLACE\b|\bCOLLATE\s+NOCASE\b|\browid\b`)
var orIgnore = regexp.MustCompile(`(?i)^(\s*INSERT)\s+OR\s+IGNORE\s+(INTO)`)

var (
	cacheMu sync.Mutex
	cache   = map[[2]string]string{}
)

// Translate turns a SQLite-style statement into PostgreSQL: `?` -> $n outside quotes, INSERT OR IGNORE -> ON CONFLICT DO NOTHING.
func Translate(sql string, withArgs bool) (string, error) {
	key := [2]string{sql, strconv.FormatBool(withArgs)}
	cacheMu.Lock()
	hit, ok := cache[key]
	cacheMu.Unlock()
	if ok {
		return hit, nil
	}
	if sqliteOnly.MatchString(sql) {
		head := sql
		if len(head) > 80 {
			head = head[:80]
		}
		return "", fmt.Errorf("SQLite-only construct in: %s", head)
	}
	var out strings.Builder
	var quote byte
	n := 0
	for i := 0; i < len(sql); i++ {
		ch := sql[i]
		switch {
		case quote != 0:
			out.WriteByte(ch)
			if ch == quote {
				quote = 0
			}
		case ch == '\'' || ch == '"':
			quote = ch
			out.WriteByte(ch)
		case ch == '?' && withArgs:
			n++
			out.WriteString("$" + strconv.Itoa(n))
		default:
			out.WriteByte(ch)
		}
	}
	text := out.String()
	if m := insertRe.FindStringSubmatch(text); m != nil && m[1] != "" {
		text = orIgnore.ReplaceAllString(text, "$1 $2")
		text = strings.TrimRight(strings.TrimRight(text, " \t\r\n"), ";") + " ON CONFLICT DO NOTHING"
	}
	cacheMu.Lock()
	if len(cache) < 2000 {
		cache[key] = text
	}
	cacheMu.Unlock()
	return text, nil
}

// DB is the pooled database.
type DB struct {
	DSN          string
	Path         string
	pool         *pgxpool.Pool
	serialTables map[string]bool
	poolSize     int
}

func safeDSN(dsn string) string {
	cfg, err := pgx.ParseConfig(dsn)
	if err != nil {
		return "(connection string not shown)"
	}
	var parts []string
	if cfg.Database != "" {
		parts = append(parts, "dbname="+cfg.Database)
	}
	if cfg.Host != "" {
		parts = append(parts, "host="+cfg.Host)
	}
	if cfg.Port != 0 {
		parts = append(parts, fmt.Sprintf("port=%d", cfg.Port))
	}
	if cfg.User != "" {
		parts = append(parts, "user="+cfg.User)
	}
	if len(parts) == 0 {
		return "(default connection)"
	}
	return strings.Join(parts, " ")
}

// Open connects (creating the schema if needed). dsn "" = FH2MP_PG_DSN, else dbname=fh2mp.
func Open(dsn string) (*DB, error) {
	if dsn == "" {
		dsn = os.Getenv("FH2MP_PG_DSN")
	}
	if dsn == "" {
		dsn = "dbname=fh2mp"
	}
	cfg, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		return nil, err
	}
	size := 10
	if v, err := strconv.Atoi(os.Getenv("FH2MP_PG_POOL")); err == nil && v > 0 {
		size = v
	}
	cfg.MaxConns = int32(size)
	// client-side binding like psycopg's ClientCursor: a NULL parameter never needs a cast
	cfg.ConnConfig.DefaultQueryExecMode = pgx.QueryExecModeSimpleProtocol
	if cfg.ConnConfig.RuntimeParams == nil {
		cfg.ConnConfig.RuntimeParams = map[string]string{}
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = "public, game"
	cfg.ConnConfig.RuntimeParams["TimeZone"] = "UTC"
	pool, err := pgxpool.NewWithConfig(context.Background(), cfg)
	if err != nil {
		return nil, err
	}
	d := &DB{DSN: dsn, Path: safeDSN(dsn), pool: pool, poolSize: size}
	if err := d.migrate(); err != nil {
		pool.Close()
		return nil, err
	}
	rows, err := d.Query(`SELECT table_name FROM information_schema.columns WHERE table_schema='public' AND column_name='id' ` +
		`AND (is_identity='YES' OR column_default LIKE 'nextval%')`)
	if err != nil {
		pool.Close()
		return nil, err
	}
	d.serialTables = map[string]bool{}
	for _, r := range rows {
		d.serialTables[r.StrAt(0)] = true
	}
	return d, nil
}

// Close closes every pooled connection.
func (d *DB) Close() { d.pool.Close() }

func convert(v any) any {
	switch x := v.(type) {
	case pgtype.Numeric:
		if !x.Valid {
			return nil
		}
		f, err := x.Float64Value()
		if err != nil {
			return float64(0)
		}
		return f.Float64
	case *big.Int:
		return x.Int64()
	case [16]byte:
		return fmt.Sprintf("%x", x)
	}
	return v
}

func collect(rows pgx.Rows) ([]Row, error) {
	defer rows.Close()
	fds := rows.FieldDescriptions()
	names := make([]string, len(fds))
	idx := make(map[string]int, len(fds))
	for i, f := range fds {
		names[i] = f.Name
		idx[strings.ToLower(f.Name)] = i
	}
	var out []Row
	for rows.Next() {
		vals, err := rows.Values()
		if err != nil {
			return nil, err
		}
		for i := range vals {
			vals[i] = convert(vals[i])
		}
		out = append(out, Row{names: names, idx: idx, Vals: vals})
	}
	return out, rows.Err()
}

type queryer interface {
	Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error)
	Exec(ctx context.Context, sql string, args ...any) (pgconn.CommandTag, error)
}

func (d *DB) run(q queryer, sql string, args []any) ([]Row, Result, error) {
	text, err := Translate(sql, len(args) > 0)
	if err != nil {
		return nil, Result{}, err
	}
	m := insertRe.FindStringSubmatch(text)
	wantID := m != nil && d.serialTables[strings.ToLower(m[2])] && !strings.Contains(strings.ToUpper(text), "RETURNING")
	if wantID {
		text += " RETURNING id"
	}
	ctx := context.Background()
	if wantID || isSelectLike(text) {
		rows, err := q.Query(ctx, text, args...)
		if err != nil {
			return nil, Result{}, err
		}
		recs, err := collect(rows)
		if err != nil {
			return nil, Result{}, err
		}
		res := Result{RowsAffected: rows.CommandTag().RowsAffected()}
		if wantID && len(recs) > 0 {
			res.LastID, res.HasLastID = recs[0].IntAt(0), true
		}
		return recs, res, nil
	}
	tag, err := q.Exec(ctx, text, args...)
	if err != nil {
		return nil, Result{}, err
	}
	return nil, Result{RowsAffected: tag.RowsAffected()}, nil
}

func isSelectLike(sql string) bool {
	s := strings.ToUpper(strings.TrimLeft(sql, " \t\r\n("))
	return strings.HasPrefix(s, "SELECT") || strings.HasPrefix(s, "WITH") || strings.Contains(s, " RETURNING ") || strings.HasPrefix(s, "VALUES") || strings.HasPrefix(s, "SHOW")
}

func (d *DB) slow(started time.Time, sql string) {
	if took := time.Since(started).Seconds(); took > SlowQuerySeconds {
		if ok, skipped := rate.Allow([2]any{"slow", sqlHead(sql, 60)}, DBLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d like it since the last line)", skipped)
			}
			logger.Warn(fmt.Sprintf("slow query %.1f s (limit %g): %s%s", took, SlowQuerySeconds, sqlHead(sql, 110), extra))
		}
	}
}

// Query runs a read and returns every row.
func (d *DB) Query(sql string, args ...any) ([]Row, error) {
	for attempt := 0; ; attempt++ {
		started := time.Now()
		rows, _, err := d.run(d.pool, sql, args)
		if err == nil {
			d.slow(started, sql)
			return rows, nil
		}
		var conn *pgconn.ConnectError
		if attempt == 0 && (errors.As(err, &conn) || pgconn.SafeToRetry(err)) {
			logger.Warn(fmt.Sprintf("connection lost after %.1f s, retrying once on another: %v; %s", time.Since(started).Seconds(), err, sqlHead(sql, 60)))
			continue
		}
		return nil, err
	}
}

// One runs a read and returns the first row (nil = none).
func (d *DB) One(sql string, args ...any) (*Row, error) {
	rows, err := d.Query(sql, args...)
	if err != nil || len(rows) == 0 {
		return nil, err
	}
	return &rows[0], nil
}

// Exec runs one statement outside a transaction.
func (d *DB) Exec(sql string, args ...any) (Result, error) {
	_, res, err := d.run(d.pool, sql, args)
	return res, err
}

// Tx is the handle of a write transaction.
type Tx struct {
	d  *DB
	tx pgx.Tx
}

// Exec runs a statement inside the transaction.
func (t *Tx) Exec(sql string, args ...any) (Result, error) {
	_, res, err := t.d.run(t.tx, sql, args)
	return res, err
}

// Query runs a read inside the transaction.
func (t *Tx) Query(sql string, args ...any) ([]Row, error) {
	rows, _, err := t.d.run(t.tx, sql, args)
	return rows, err
}

// One runs a read inside the transaction and returns the first row (nil = none).
func (t *Tx) One(sql string, args ...any) (*Row, error) {
	rows, err := t.Query(sql, args...)
	if err != nil || len(rows) == 0 {
		return nil, err
	}
	return &rows[0], nil
}

// Do runs fn as a write transaction (COMMIT, or ROLLBACK if fn fails). It takes the advisory lock first, so writers never interleave.
func (d *DB) Do(fn func(*Tx) error) error {
	ctx := context.Background()
	asked := time.Now()
	tx, err := d.pool.Begin(ctx)
	if err != nil {
		return err
	}
	var got time.Time
	defer func() {
		if !got.IsZero() && time.Since(got).Seconds() > SlowTxSeconds {
			if ok, skipped := rate.Allow("tx-long", DBLogSeconds); ok {
				extra := ""
				if skipped > 0 {
					extra = fmt.Sprintf(" (+%d like it since the last line)", skipped)
				}
				logger.Warn(fmt.Sprintf("a write transaction held the write lock for %.1f s (limit %g); every other writer waited%s", time.Since(got).Seconds(), SlowTxSeconds, extra))
			}
		}
	}()
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock($1)", AdvisoryLock); err != nil {
		tx.Rollback(ctx)
		return err
	}
	got = time.Now()
	if got.Sub(asked).Seconds() > SlowLockSeconds {
		if ok, skipped := rate.Allow("lock-wait", DBLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			logger.Warn(fmt.Sprintf("waited %.1f s for the write lock, another write transaction held it%s", got.Sub(asked).Seconds(), extra))
		}
	}
	if err := fn(&Tx{d: d, tx: tx}); err != nil {
		tx.Rollback(ctx)
		return err
	}
	return tx.Commit(ctx)
}

// ---- schema ----

var stmtComment = regexp.MustCompile(`--.*$`)

// Statements splits a migration script into statements: `--` comments dropped, then split on `;`.
func Statements(script string) []string {
	lines := strings.Split(script, "\n")
	for i, ln := range lines {
		lines[i] = stmtComment.ReplaceAllString(strings.TrimRight(ln, "\r"), "")
	}
	var out []string
	for _, part := range strings.Split(strings.Join(lines, "\n"), ";") {
		if p := strings.TrimSpace(part); p != "" {
			out = append(out, p)
		}
	}
	return out
}

// Migrations lists the embedded migration scripts in order.
func Migrations() []string {
	entries, _ := migrationFS.ReadDir("migrations")
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		names = append(names, e.Name())
	}
	sort.Strings(names)
	out := make([]string, len(names))
	for i, n := range names {
		b, _ := migrationFS.ReadFile("migrations/" + n)
		out[i] = string(b)
	}
	return out
}

func (d *DB) migrate() error {
	started := time.Now()
	ctx := context.Background()
	migs := Migrations()
	conn, err := d.pool.Acquire(ctx)
	if err != nil {
		return err
	}
	defer conn.Release()
	tx, err := conn.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock($1)", AdvisoryLock+1); err != nil {
		return err
	}
	if _, err := tx.Exec(ctx, "CREATE TABLE IF NOT EXISTS schema_version (version BIGINT NOT NULL)"); err != nil {
		return err
	}
	version := 0
	rows, err := tx.Query(ctx, "SELECT version FROM schema_version")
	if err != nil {
		return err
	}
	if rows.Next() {
		var v int64
		if err := rows.Scan(&v); err != nil {
			rows.Close()
			return err
		}
		version = int(v)
	}
	rows.Close()
	if version > len(migs) {
		return fmt.Errorf("%s is schema v%d, this build only knows v%d", d.Path, version, len(migs))
	}
	for v := version; v < len(migs); v++ {
		for _, stmt := range Statements(migs[v]) {
			if _, err := tx.Exec(ctx, stmt); err != nil {
				return fmt.Errorf("migration %d: %w", v+1, err)
			}
		}
		if err := afterMigration(tx, v+1); err != nil {
			return err
		}
	}
	if version == 0 {
		if _, err := tx.Exec(ctx, "INSERT INTO schema_version (version) VALUES ($1)", len(migs)); err != nil {
			return err
		}
	} else if version < len(migs) {
		if _, err := tx.Exec(ctx, "UPDATE schema_version SET version=$1", len(migs)); err != nil {
			return err
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return err
	}
	if version < len(migs) {
		logger.Info(fmt.Sprintf("schema migrated from v%d to v%d (%d step(s), %.1f s) on %s", version, len(migs), len(migs)-version, time.Since(started).Seconds(), d.Path))
	}
	return nil
}

// Migration 15: the old keyword ids -> positions in the lists of fh2mp_lsp.KEYWORD_LISTS.
var keywordRemap = []struct {
	kind  string
	first int64
	ids   map[int64]int64
}{
	{"livery", 9, map[int64]int64{1: 1, 3: 19, 4: 14, 5: 13, 12: 4, 13: 15, 16: 11, 17: 2}},
	{"photo", 5, map[int64]int64{1: 1, 15: 4}},
	{"tune", 7, map[int64]int64{1: 6, 3: 4, 4: 5, 7: 3, 8: 1, 9: 7, 13: 13, 14: 15, 16: 6}},
}

func afterMigration(tx pgx.Tx, version int) error {
	ctx := context.Background()
	if version == 15 {
		for _, k := range keywordRemap {
			rows, err := tx.Query(ctx, "SELECT id, keyword1, keyword2 FROM ugc_files WHERE kind=$1 AND (keyword1<>0 OR keyword2<>0)", k.kind)
			if err != nil {
				return err
			}
			type rec struct{ id, k1, k2 int64 }
			var recs []rec
			for rows.Next() {
				var r rec
				if err := rows.Scan(&r.id, &r.k1, &r.k2); err != nil {
					rows.Close()
					return err
				}
				recs = append(recs, r)
			}
			rows.Close()
			for _, r := range recs {
				var new1, new2 int64
				for _, old := range []int64{r.k1, r.k2} {
					n := k.ids[old]
					if n != 0 && n <= k.first && new1 == 0 {
						new1 = n
					} else if n > k.first && new2 == 0 {
						new2 = n
					}
				}
				if _, err := tx.Exec(ctx, "UPDATE ugc_files SET keyword1=$1, keyword2=$2 WHERE id=$3", new1, new2, r.id); err != nil {
					return err
				}
			}
		}
	}
	if version == 1 {
		if _, err := tx.Exec(ctx, "SELECT setval(pg_get_serial_sequence('notifications', 'id'), $1)", time.Now().Unix()-1); err != nil {
			return err
		}
	}
	return nil
}

// SetSequence makes the next id handed out for table value+1.
func SetSequence(t *Tx, table string, value int64, column string) error {
	if column == "" {
		column = "id"
	}
	flag := "false"
	if value >= 1 {
		flag = "true"
	}
	v := value
	if v < 1 {
		v = 1
	}
	_, err := t.Exec(fmt.Sprintf("SELECT setval(pg_get_serial_sequence('%s', '%s'), %d, %s)", table, column, v, flag))
	return err
}

// MaxLevel is the highest level.
const MaxLevel = 999

// CleanLevel sanitises the wire currentCareerLevel (cumulative career XP, a u32); ok=false means "not given".
func CleanLevel(level *int64) (int64, bool) {
	if level == nil {
		return 0, false
	}
	return int64(math.Min(math.Max(float64(*level), 0), 0xFFFFFFFF)), true
}

// DisplayLevel is the stock FH2 XP-to-level curve, for human-facing APIs only.
func DisplayLevel(careerXP int64) int64 {
	xp, _ := CleanLevel(&careerXP)
	if xp < 210000 {
		return 1 + xp/15000
	}
	if xp < 2910000 {
		return 15 + (xp-210000)/20000
	}
	lo, hi := int64(0), int64(849)
	for lo < hi {
		mid := (lo + hi + 1) / 2
		if 2910000+20000*mid+150*mid*(mid+1) <= xp {
			lo = mid
		} else {
			hi = mid - 1
		}
	}
	return 150 + lo
}

// TouchPlayer creates the player row or refreshes last_seen (and the gamertag / career level when given).
func TouchPlayer(t *Tx, xuid int64, tag string, seen *float64, level *int64) error {
	s := float64(time.Now().UnixNano()) / 1e9
	if seen != nil {
		s = *seen
	}
	var lv int64
	var lvArg any
	if l, ok := CleanLevel(level); ok {
		lv, lvArg = l, l
	}
	_, err := t.Exec("INSERT INTO players (xuid, gamertag, career_level, first_seen, last_seen) VALUES (?,?,?,?,?) "+
		"ON CONFLICT(xuid) DO UPDATE SET last_seen=excluded.last_seen, "+
		"gamertag=CASE WHEN excluded.gamertag<>'' THEN excluded.gamertag ELSE players.gamertag END, "+
		"career_level=CASE WHEN ? IS NULL THEN players.career_level ELSE excluded.career_level END",
		xuid, tag, lv, s, s, lvArg)
	return err
}

// MetaGet reads a key of the meta table.
func (d *DB) MetaGet(key, def string) (string, error) {
	r, err := d.One("SELECT value FROM meta WHERE key=?", key)
	if err != nil || r == nil {
		return def, err
	}
	return r.Str("value"), nil
}
