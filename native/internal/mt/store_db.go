package mt

import (
	"time"

	"fh2mp/internal/db"
)

// UpdateTTL is how long a stored update waits for its client at most (a sealed message's bytes too).
const UpdateTTL = 7 * 24 * time.Hour

// DBStore is the UpdateStore on the LSP database (tables mt_state and mt_updates, migrations 19 and 20).
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

func (s *DBStore) Append(x int64, body []byte) (int, error) { return s.AppendChat(x, body, 0, false) }

func (s *DBStore) AppendChat(x int64, body []byte, chat int32, message bool) (int, error) {
	var pts int
	var tag any
	if chat != 0 {
		tag = int64(chat)
	}
	err := s.DB.Do(func(t *db.Tx) error {
		r, err := t.One("INSERT INTO mt_state (xuid, pts) VALUES (?, 1) ON CONFLICT (xuid) DO UPDATE SET pts = mt_state.pts + 1 RETURNING pts", x)
		if err != nil {
			return err
		}
		pts = int(r.Int("pts"))
		_, err = t.Exec("INSERT INTO mt_updates (xuid, pts, created, body, e2e_chat, e2e_message) VALUES (?, ?, ?, ?, ?, ?)", x, pts,
			float64(s.now().UnixNano())/1e9, body, tag, message && chat != 0)
		return err
	})
	return pts, err
}

func (s *DBStore) Since(x int64, after, limit int) ([]StoredUpdate, int, error) {
	rows, err := s.DB.Query("SELECT pts, body FROM mt_updates WHERE xuid=? AND pts>? ORDER BY pts LIMIT ?", x, after, limit)
	if err != nil {
		return nil, 0, err
	}
	g, err := s.DB.One("SELECT gone FROM mt_state WHERE xuid=?", x)
	if err != nil {
		return nil, 0, err
	}
	gone := 0
	if g != nil {
		gone = int(g.Int("gone"))
	}
	out := make([]StoredUpdate, len(rows))
	for i := range rows {
		out[i] = StoredUpdate{Pts: int(rows[i].Int("pts")), Body: rows[i].Bytes("body")}
	}
	return out, gone, nil
}

func (s *DBStore) Ack(x int64, pts int) error {
	return s.DB.Do(func(t *db.Tx) error {
		r, err := t.One("SELECT MAX(pts) FROM mt_updates WHERE xuid=? AND pts<=? AND e2e_chat IS NULL", x, pts)
		if err != nil || r == nil || r.IsNullAt(0) {
			return err
		}
		if _, err := t.Exec("DELETE FROM mt_updates WHERE xuid=? AND pts<=? AND e2e_chat IS NULL", x, pts); err != nil {
			return err
		}
		_, err = t.Exec("UPDATE mt_state SET gone = GREATEST(gone, ?) WHERE xuid=?", r.IntAt(0), x)
		return err
	})
}

func (s *DBStore) AckChat(x int64, pts int) (int64, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("DELETE FROM mt_updates WHERE xuid=? AND pts<=? AND e2e_chat IS NOT NULL", x, pts)
		n = res.RowsAffected
		return err
	})
	return n, err
}

func (s *DBStore) DropChat(chat int32) (int64, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("DELETE FROM mt_updates WHERE e2e_chat=? AND e2e_message", int64(chat))
		n = res.RowsAffected
		return err
	})
	return n, err
}

// Expire drops stored updates older than UpdateTTL (a sealed message's bytes with them) and raises each account's gone past them.
func (s *DBStore) Expire() (int64, error) {
	var n int64
	cut := float64(s.now().Add(-UpdateTTL).UnixNano()) / 1e9
	err := s.DB.Do(func(t *db.Tx) error {
		if _, err := t.Exec("UPDATE mt_state SET gone = GREATEST(mt_state.gone, e.m) FROM (SELECT xuid, MAX(pts) AS m FROM mt_updates WHERE created < ? "+
			"GROUP BY xuid) e WHERE mt_state.xuid = e.xuid", cut); err != nil {
			return err
		}
		res, err := t.Exec("DELETE FROM mt_updates WHERE created < ?", cut)
		n = res.RowsAffected
		return err
	})
	return n, err
}
