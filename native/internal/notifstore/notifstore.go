// Package notifstore keeps the Message Center notifications (tables notifications + notification_receipts) (= server/notifstore.py). The row is
// immutable content; what one player did with it is a receipt row, so a broadcast one player clears is unchanged for the others.
package notifstore

import (
	"fmt"
	"strings"
	"sync/atomic"
	"time"

	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
)

var log = diag.Get("notif")

const selectSQL = "SELECT n.id, n.type, n.message, n.sra, n.sent, n.xuid, n.car_id, n.params, " +
	"r.read_at, r.cleared_at, r.claimed_at FROM notifications n " +
	"LEFT JOIN notification_receipts r ON r.notification_id = n.id AND r.xuid = ?"

// Filetime is a Windows FILETIME; unix 0 = now.
func Filetime(unix float64) int64 {
	if unix == 0 {
		unix = float64(time.Now().UnixNano()) / 1e9
	}
	return int64((unix + 11644473600) * 10_000_000)
}

// Notification is one Message Center entry as the API and the wire show it.
type Notification struct {
	ID      int64
	Type    int64
	Message string
	Sra     string
	Sent    int64
	Xuid    int64
	CarID   *int64
	Params  pyjson.Obj
	Read    bool
	Claimed bool
	ReadBy  []int64
	HasRead bool // ReadBy is filled (admin listing)
}

// JSON is the dict the Python store returns, in its key order.
func (n *Notification) JSON() pyjson.Obj {
	o := pyjson.Obj{{K: "id", V: n.ID}, {K: "type", V: n.Type}, {K: "message", V: n.Message}, {K: "sra", V: n.Sra}, {K: "sent", V: n.Sent},
		{K: "xuid", V: n.Xuid}, {K: "params", V: n.Params}, {K: "read", V: n.Read}, {K: "claimed", V: n.Claimed}}
	if n.CarID != nil {
		o = append(o, pyjson.Pair{K: "carId", V: *n.CarID})
	}
	if n.HasRead {
		o = append(o, pyjson.Pair{K: "readBy", V: n.ReadBy})
	}
	return o
}

func fromRow(r *db.Row) (*Notification, error) {
	pv, err := pyjson.Parse(r.Str("params"))
	if err != nil {
		return nil, err
	}
	params, _ := pv.(pyjson.Obj)
	n := &Notification{ID: r.Int("id"), Type: r.Int("type"), Message: r.Str("message"), Sra: r.Str("sra"), Sent: r.Int("sent"), Xuid: r.Int("xuid"),
		Params: params, Read: !r.IsNull("read_at"), Claimed: !r.IsNull("claimed_at")}
	if !r.IsNull("car_id") {
		c := r.Int("car_id")
		n.CarID = &c
	}
	return n, nil
}

// Store is the notification store.
type Store struct {
	DB         *db.DB
	generation atomic.Int64
}

// New makes a store.
func New(d *db.DB) *Store { return &Store{DB: d} }

// Generation counts every change, so a reply built for an older value can be reused only while nothing happened.
func (s *Store) Generation() int64 { return s.generation.Load() }

func giftIDSet(o pyjson.Obj) bool {
	v, ok := o.Get("GiftId")
	if !ok {
		return false
	}
	switch x := v.(type) {
	case nil:
		return false
	case int64:
		return x != 0
	case float64:
		return x != 0
	case string:
		return x != ""
	case bool:
		return x
	}
	return true
}

// Add queues a notification; giftID fills params["GiftId"] from the new id.
func (s *Store) Add(message, sra string, ntype int64, params pyjson.Obj, xuid int64, carID *int64, giftID bool) (*Notification, error) {
	var out *Notification
	err := s.DB.Do(func(t *db.Tx) error {
		if params == nil {
			params = pyjson.Obj{}
		}
		text, err := pyjson.Dumps(params, pyjson.Opts{})
		if err != nil {
			return err
		}
		var car any
		if carID != nil {
			car = *carID
		}
		res, err := t.Exec("INSERT INTO notifications (type, message, sra, sent, xuid, car_id, params) VALUES (?,?,?,?,?,?,?)", ntype, message, sra, Filetime(0), xuid, car, text)
		if err != nil {
			return err
		}
		nid := res.LastID
		if giftID && !giftIDSet(params) {
			p := append(pyjson.Obj(nil), params...)
			p.Set("GiftId", nid&0x7FFFFFFF)
			text, _ = pyjson.Dumps(p, pyjson.Opts{})
			if _, err := t.Exec("UPDATE notifications SET params=? WHERE id=?", text, nid); err != nil {
				return err
			}
		}
		r, err := t.One(selectSQL+" WHERE n.id=?", 0, nid)
		if err != nil {
			return err
		}
		out, err = fromRow(r)
		return err
	})
	if err != nil {
		return nil, err
	}
	s.generation.Add(1)
	return out, nil
}

// Visible is what a player's Message Center shows: broadcasts plus their own, minus what they cleared.
func (s *Store) Visible(xuid int64) ([]*Notification, error) {
	rows, err := s.DB.Query(selectSQL+" WHERE (n.xuid = 0 OR n.xuid = ?) AND r.cleared_at IS NULL ORDER BY n.id", xuid, xuid)
	if err != nil {
		return nil, err
	}
	var out []*Notification
	for i := range rows {
		n, err := fromRow(&rows[i])
		if err != nil {
			return nil, err
		}
		out = append(out, n)
	}
	return out, nil
}

// MarkRead records that a player read a notification.
func (s *Store) MarkRead(xuid, nid int64) error {
	_, err := s.receipt(xuid, nid, "read_at")
	return err
}

// Clear records that a player cleared a notification.
func (s *Store) Clear(xuid, nid int64) error {
	_, err := s.receipt(xuid, nid, "cleared_at")
	return err
}

// Claim marks a gift as retrieved by this player: true exactly once per (gift, player), even when two requests race.
func (s *Store) Claim(xuid, nid int64) (bool, error) {
	n, err := s.receipt(xuid, nid, "claimed_at")
	if err != nil {
		return false, err
	}
	if n != 1 {
		log.Debug(fmt.Sprintf("claim of notification %d by xuid %#x did not count: it was claimed before, or is not theirs / gone", nid, xuid))
	}
	return n == 1, nil
}

func (s *Store) receipt(xuid, nid int64, column string) (int64, error) {
	now := float64(time.Now().UnixNano()) / 1e9
	s.generation.Add(1)
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		r, err := t.One("SELECT 1 FROM notifications WHERE id=? AND (xuid=0 OR xuid=?)", nid, xuid)
		if err != nil {
			return err
		}
		if r == nil {
			log.Debug(fmt.Sprintf("%s of notification %d by xuid %#x ignored: not theirs, or deleted", strings.Split(column, "_")[0], nid, xuid))
			return nil
		}
		res, err := t.Exec(fmt.Sprintf("INSERT INTO notification_receipts (notification_id, xuid, %s) VALUES (?,?,?) ON CONFLICT(notification_id, xuid) DO UPDATE SET %s=excluded.%s WHERE notification_receipts.%s IS NULL",
			column, column, column, column), nid, xuid, now)
		n = res.RowsAffected
		return err
	})
	return n, err
}

// All lists every notification with the xuids that read it.
func (s *Store) All() ([]*Notification, error) {
	rows, err := s.DB.Query("SELECT id, type, message, sra, sent, xuid, car_id, params, NULL AS read_at, NULL AS cleared_at, NULL AS claimed_at FROM notifications ORDER BY id")
	if err != nil {
		return nil, err
	}
	var out []*Notification
	for i := range rows {
		n, err := fromRow(&rows[i])
		if err != nil {
			return nil, err
		}
		rb, err := s.DB.Query("SELECT xuid FROM notification_receipts WHERE notification_id=? AND read_at IS NOT NULL", n.ID)
		if err != nil {
			return nil, err
		}
		n.ReadBy = []int64{}
		for _, r := range rb {
			n.ReadBy = append(n.ReadBy, r.IntAt(0))
		}
		n.HasRead = true
		n.Read = len(n.ReadBy) > 0
		out = append(out, n)
	}
	return out, nil
}

// Get finds one notification (nil = none).
func (s *Store) Get(nid int64) (*Notification, error) {
	all, err := s.All()
	for _, n := range all {
		if n.ID == nid {
			return n, err
		}
	}
	return nil, err
}

// SetCarID sets the car of a notification.
func (s *Store) SetCarID(nid int64, carID *int64) error {
	s.generation.Add(1)
	return s.DB.Do(func(t *db.Tx) error {
		var c any
		if carID != nil {
			c = *carID
		}
		_, err := t.Exec("UPDATE notifications SET car_id=? WHERE id=?", c, nid)
		return err
	})
}

// Delete removes a notification.
func (s *Store) Delete(nid int64) (bool, error) {
	s.generation.Add(1)
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		r, err := t.Exec("DELETE FROM notifications WHERE id=?", nid)
		n = r.RowsAffected
		return err
	})
	return n > 0, err
}

// DeleteAll removes every notification.
func (s *Store) DeleteAll() (int64, error) {
	s.generation.Add(1)
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		r, err := t.Exec("DELETE FROM notifications")
		n = r.RowsAffected
		return err
	})
	return n, err
}

// Count is the number of notifications.
func (s *Store) Count() (int64, error) {
	r, err := s.DB.One("SELECT COUNT(*) FROM notifications")
	if err != nil {
		return 0, err
	}
	return r.IntAt(0), nil
}
