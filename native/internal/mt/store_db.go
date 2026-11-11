package mt

import (
	"time"

	"fh2mp/internal/db"
)

// UpdateTTL is how long a stored update waits for its client at most.
const UpdateTTL = 7 * 24 * time.Hour

// DBStore is the UpdateStore on the LSP database (tables mt_state and mt_updates, migration 16).
type DBStore struct {
	DB  *db.DB
	Now func() time.Time
}

func (s *DBStore) now() time.Time {
	if s.Now != nil {
		return s.Now()
	}
	return time.Now()
}

func (s *DBStore) State(x int64) (int, error) {
	r, err := s.DB.One("SELECT pts FROM mt_state WHERE xuid=?", x)
	if err != nil || r == nil {
		return 0, err
	}
	return int(r.Int("pts")), nil
}

func (s *DBStore) Append(x int64, body []byte) (int, error) {
	var pts int
	err := s.DB.Do(func(t *db.Tx) error {
		r, err := t.One("INSERT INTO mt_state (xuid, pts) VALUES (?, 1) ON CONFLICT (xuid) DO UPDATE SET pts = mt_state.pts + 1 RETURNING pts", x)
		if err != nil {
			return err
		}
		pts = int(r.Int("pts"))
		_, err = t.Exec("INSERT INTO mt_updates (xuid, pts, created, body) VALUES (?, ?, ?, ?)", x, pts, float64(s.now().UnixNano())/1e9, body)
		return err
	})
	return pts, err
}

func (s *DBStore) Since(x int64, after, limit int) ([][]byte, int, error) {
	rows, err := s.DB.Query("SELECT pts, body FROM mt_updates WHERE xuid=? AND pts>? ORDER BY pts LIMIT ?", x, after, limit)
	if err != nil {
		return nil, 0, err
	}
	if len(rows) == 0 {
		return nil, after + 1, nil
	}
	out := make([][]byte, len(rows))
	for i := range rows {
		out[i] = rows[i].Bytes("body")
	}
	return out, int(rows[0].Int("pts")), nil
}

func (s *DBStore) Ack(x int64, pts int) error {
	return s.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("DELETE FROM mt_updates WHERE xuid=? AND pts<=?", x, pts)
		return err
	})
}

// Expire drops stored updates older than UpdateTTL; returns how many went.
func (s *DBStore) Expire() (int64, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("DELETE FROM mt_updates WHERE created < ?", float64(s.now().Add(-UpdateTTL).UnixNano())/1e9)
		n = res.RowsAffected
		return err
	})
	return n, err
}
