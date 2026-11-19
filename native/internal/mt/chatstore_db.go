package mt

import (
	"time"

	"fh2mp/internal/db"
)

// DBChatStore is the ChatStore on the LSP database (migration 20: mt_chat_devices, mt_e2e_chats, mt_e2e_sent, mt_e2e_reports).
type DBChatStore struct{ DB *db.DB }

func unix(t time.Time) float64 { return float64(t.UnixNano()) / 1e9 }

func (s *DBChatStore) Device(user int64) (*ChatDevice, error) {
	r, err := s.DB.One("SELECT xuid, device_id, pubkey, account_key, signature, date FROM mt_chat_devices WHERE xuid=?", user)
	if err != nil || r == nil {
		return nil, err
	}
	return &ChatDevice{User: r.Int("xuid"), Device: r.Int("device_id"), Pubkey: r.Bytes("pubkey"), AccountKey: r.Bytes("account_key"),
		Signature: r.Bytes("signature"), Date: int(r.Int("date"))}, nil
}

func (s *DBChatStore) SetDevice(d ChatDevice) (*ChatDevice, error) {
	var old *ChatDevice
	err := s.DB.Do(func(t *db.Tx) error {
		r, err := t.One("SELECT xuid, device_id, pubkey, account_key, signature, date FROM mt_chat_devices WHERE xuid=?", d.User)
		if err != nil {
			return err
		}
		if r != nil {
			old = &ChatDevice{User: r.Int("xuid"), Device: r.Int("device_id"), Pubkey: r.Bytes("pubkey"), AccountKey: r.Bytes("account_key"),
				Signature: r.Bytes("signature"), Date: int(r.Int("date"))}
		}
		_, err = t.Exec("INSERT INTO mt_chat_devices (xuid, device_id, pubkey, account_key, signature, date) VALUES (?, ?, ?, ?, ?, ?) "+
			"ON CONFLICT (xuid) DO UPDATE SET device_id=excluded.device_id, pubkey=excluded.pubkey, account_key=excluded.account_key, "+
			"signature=excluded.signature, date=excluded.date", d.User, d.Device, d.Pubkey, d.AccountKey, d.Signature, d.Date)
		return err
	})
	return old, err
}

const chatCols = "id, date, admin, admin_device, participant, participant_device, random_id, state, g_a, g_a_sig, g_b, g_b_sig, key_fingerprint, changed"

func chatFromRow(r *db.Row) SecretChat {
	c := SecretChat{ID: int32(r.Int("id")), Date: int(r.Int("date")), Admin: r.Int("admin"), AdminDevice: r.Int("admin_device"),
		Participant: r.Int("participant"), ParticipantDevice: r.Int("participant_device"), RandomID: int32(r.Int("random_id")),
		State: ChatState(r.Int("state")), GA: r.Bytes("g_a"), GASig: r.Bytes("g_a_sig"), GB: r.Bytes("g_b"), GBSig: r.Bytes("g_b_sig"),
		Fingerprint: r.Int("key_fingerprint")}
	f := r.Float("changed")
	c.changed = time.Unix(0, int64(f*1e9))
	return c
}

func (s *DBChatStore) NewChat(c SecretChat, now time.Time) (SecretChat, bool, error) {
	var out SecretChat
	made := false
	err := s.DB.Do(func(t *db.Tx) error {
		rows, err := t.Query("SELECT "+chatCols+" FROM mt_e2e_chats WHERE admin=? AND random_id=? AND participant=?", c.Admin, int64(c.RandomID), c.Participant)
		if err != nil {
			return err
		}
		for i := range rows {
			if o := chatFromRow(&rows[i]); string(o.GA) == string(c.GA) {
				out = o
				return nil
			}
		}
		r, err := t.One("INSERT INTO mt_e2e_chats (date, admin, admin_device, participant, participant_device, random_id, state, g_a, g_a_sig, changed) "+
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING "+chatCols, c.Date, c.Admin, c.AdminDevice, c.Participant, c.ParticipantDevice,
			int64(c.RandomID), int64(ChatRequested), c.GA, c.GASig, unix(now))
		if err != nil {
			return err
		}
		out, made = chatFromRow(r), true
		return nil
	})
	return out, made, err
}

func (s *DBChatStore) Chat(id int32) (*SecretChat, error) {
	r, err := s.DB.One("SELECT "+chatCols+" FROM mt_e2e_chats WHERE id=?", int64(id))
	if err != nil || r == nil {
		return nil, err
	}
	c := chatFromRow(r)
	return &c, nil
}

func (s *DBChatStore) Accept(id int32, gB, gBSig []byte, fp int64, now time.Time) (bool, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("UPDATE mt_e2e_chats SET state=?, g_b=?, g_b_sig=?, key_fingerprint=?, changed=? WHERE id=? AND state=?",
			int64(ChatAccepted), gB, gBSig, fp, unix(now), int64(id), int64(ChatRequested))
		n = res.RowsAffected
		return err
	})
	return n == 1, err
}

func (s *DBChatStore) Discard(id int32, now time.Time) (bool, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("UPDATE mt_e2e_chats SET state=?, changed=? WHERE id=? AND state<>?", int64(ChatDiscarded), unix(now), int64(id), int64(ChatDiscarded))
		n = res.RowsAffected
		return err
	})
	return n == 1, err
}

func (s *DBChatStore) OpenChats(user, other int64) ([]SecretChat, error) {
	q := "SELECT " + chatCols + " FROM mt_e2e_chats WHERE state<>? AND (admin=? OR participant=?)"
	args := []any{int64(ChatDiscarded), user, user}
	if other != 0 {
		q += " AND (admin=? OR participant=?)"
		args = append(args, other, other)
	}
	rows, err := s.DB.Query(q+" ORDER BY id", args...)
	if err != nil {
		return nil, err
	}
	out := make([]SecretChat, len(rows))
	for i := range rows {
		out[i] = chatFromRow(&rows[i])
	}
	return out, nil
}

func (s *DBChatStore) AddSent(m SentRecord, now time.Time) (int, bool, error) {
	date, fresh := m.Date, false
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("INSERT INTO mt_e2e_sent (chat, sender, random_id, recipient, commitment, date, created) VALUES (?, ?, ?, ?, ?, ?, ?) "+
			"ON CONFLICT DO NOTHING", int64(m.Chat), m.Sender, m.RandomID, m.Recipient, m.Commitment, m.Date, unix(now))
		if err != nil {
			return err
		}
		if res.RowsAffected == 1 {
			fresh = true
			return nil
		}
		r, err := t.One("SELECT date FROM mt_e2e_sent WHERE chat=? AND sender=? AND random_id=?", int64(m.Chat), m.Sender, m.RandomID)
		if err != nil || r == nil {
			return err
		}
		date = int(r.Int("date"))
		return nil
	})
	return date, fresh, err
}

func (s *DBChatStore) DeleteSent(chat int32, sender, randomID int64) error {
	return s.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("DELETE FROM mt_e2e_sent WHERE chat=? AND sender=? AND random_id=?", int64(chat), sender, randomID)
		return err
	})
}

func (s *DBChatStore) SentTo(chat int32, randomID, recipient int64) (*SentRecord, error) {
	r, err := s.DB.One("SELECT chat, sender, random_id, recipient, commitment, date FROM mt_e2e_sent WHERE chat=? AND random_id=? AND recipient=?",
		int64(chat), randomID, recipient)
	if err != nil || r == nil {
		return nil, err
	}
	return &SentRecord{Chat: int32(r.Int("chat")), Sender: r.Int("sender"), RandomID: r.Int("random_id"), Recipient: r.Int("recipient"),
		Commitment: r.Bytes("commitment"), Date: int(r.Int("date"))}, nil
}

func (s *DBChatStore) AddReport(rp Report) (bool, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("INSERT INTO mt_e2e_reports (created, reporter, sender, chat, random_id, text, reason) VALUES (?, ?, ?, ?, ?, ?, ?) "+
			"ON CONFLICT DO NOTHING", unix(rp.Created), rp.Reporter, rp.Sender, int64(rp.Chat), rp.RandomID, rp.Text, rp.Reason)
		n = res.RowsAffected
		return err
	})
	return n == 1, err
}

func (s *DBChatStore) Reports(limit int) ([]Report, error) {
	rows, err := s.DB.Query("SELECT id, created, reporter, sender, chat, random_id, text, reason FROM mt_e2e_reports ORDER BY id DESC LIMIT ?", limit)
	if err != nil {
		return nil, err
	}
	out := make([]Report, len(rows))
	for i, r := range rows {
		out[i] = Report{ID: r.Int("id"), Created: time.Unix(0, int64(r.Float("created")*1e9)), Reporter: r.Int("reporter"), Sender: r.Int("sender"),
			Chat: int32(r.Int("chat")), RandomID: r.Int("random_id"), Text: r.Str("text"), Reason: r.Str("reason")}
	}
	return out, nil
}

func (s *DBChatStore) Sweep(now time.Time) (int64, int64, error) {
	var sent, chats int64
	cut := unix(now.Add(-SentTTL))
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("DELETE FROM mt_e2e_sent WHERE created < ?", cut)
		if err != nil {
			return err
		}
		sent = res.RowsAffected
		res, err = t.Exec("DELETE FROM mt_e2e_chats WHERE state=? AND changed < ?", int64(ChatDiscarded), cut)
		chats = res.RowsAffected
		return err
	})
	return sent, chats, err
}
