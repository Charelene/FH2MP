package mt

import (
	"testing"
	"time"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
)

func pts(rows []StoredUpdate) []int {
	out := make([]int, len(rows))
	for i, r := range rows {
		out[i] = r.Pts
	}
	return out
}

func storeContract(t *testing.T, s UpdateStore) {
	const x = 0x9000000000100
	if p, err := s.State(x); err != nil || p != 0 {
		t.Fatalf("fresh State %d %v", p, err)
	}
	for i := 1; i <= 3; i++ {
		p, err := s.Append(x, []byte{byte(i), 0, 0, 0})
		if err != nil || p != i {
			t.Fatalf("Append %d: pts %d %v", i, p, err)
		}
	}
	if p, _ := s.State(x); p != 3 {
		t.Fatalf("State %d after 3 appends", p)
	}
	rows, gone, err := s.Since(x, 0, 2)
	if err != nil || len(rows) != 2 || rows[0].Pts != 1 || rows[1].Body[0] != 2 || gone != 0 {
		t.Fatalf("Since(0, 2): %v gone %d %v", rows, gone, err)
	}
	if err := s.Ack(x, 2); err != nil {
		t.Fatal(err)
	}
	rows, gone, _ = s.Since(x, 0, 10)
	if len(rows) != 1 || rows[0].Pts != 3 || rows[0].Body[0] != 3 || gone != 2 {
		t.Fatalf("Since(0) after Ack(2): %v gone %d", pts(rows), gone)
	}
	rows, _, _ = s.Since(x, 3, 10)
	if len(rows) != 0 {
		t.Fatalf("Since(3): %v", rows)
	}
	// Another account has its own sequence.
	if p, _ := s.Append(x+1, []byte{9, 0, 0, 0}); p != 1 {
		t.Fatalf("second account starts at pts %d", p)
	}
	if err := s.Ack(x, 3); err != nil {
		t.Fatal(err)
	}
	if p, _ := s.Append(x, []byte{4, 0, 0, 0}); p != 4 {
		t.Fatalf("pts after all rows were acked: %d, 4 expected (the counter never goes back)", p)
	}
	rows, _, _ = s.Since(x, 3, 10)
	if len(rows) != 1 || rows[0].Pts != 4 {
		t.Fatalf("Since(3) after the 4th: %v", pts(rows))
	}

	// Secret-chat rows: 5 a chat update, 6 and 7 sealed messages of chat 11, 8 a notice, 9 a sealed message of chat 12.
	const y = 0x9000000000200
	for i := 1; i <= 4; i++ {
		s.Append(y, []byte{byte(i), 0, 0, 0})
	}
	s.Ack(y, 4)
	s.AppendChat(y, []byte{5, 0, 0, 0}, 11, false)
	s.AppendChat(y, []byte{6, 0, 0, 0}, 11, true)
	s.AppendChat(y, []byte{7, 0, 0, 0}, 11, true)
	s.Append(y, []byte{8, 0, 0, 0})
	s.AppendChat(y, []byte{9, 0, 0, 0}, 12, true)
	// An acked push keeps the secret-chat rows: a client that lost them can ask again from its pts.
	if err := s.Ack(y, 9); err != nil {
		t.Fatal(err)
	}
	rows, gone, _ = s.Since(y, 4, 10)
	if got := pts(rows); len(got) != 4 || got[0] != 5 || got[3] != 9 || gone != 8 {
		t.Fatalf("after Ack(9): rows %v gone %d (5,6,7,9 and gone 8 expected)", got, gone)
	}
	// A discard of chat 11 drops its sealed messages only, in every account.
	s.AppendChat(x, []byte{10, 0, 0, 0}, 11, true)
	if n, err := s.DropChat(11); err != nil || n != 3 {
		t.Fatalf("DropChat(11): %d %v", n, err)
	}
	rows, _, _ = s.Since(y, 4, 10)
	if got := pts(rows); len(got) != 2 || got[0] != 5 || got[1] != 9 {
		t.Fatalf("after DropChat(11): %v", got)
	}
	// messages.ackEncrypted up to 6: the chat update at 5 goes, the message at 9 stays; gone does not move (the client has them).
	if n, err := s.AckChat(y, 6); err != nil || n != 1 {
		t.Fatalf("AckChat(6): %d %v", n, err)
	}
	rows, gone, _ = s.Since(y, 0, 10)
	if got := pts(rows); len(got) != 1 || got[0] != 9 || gone != 8 {
		t.Fatalf("after AckChat(6): %v gone %d", got, gone)
	}
	if n, _ := s.AckChat(y, 9); n != 1 {
		t.Fatalf("AckChat(9) dropped %d", n)
	}
	if p, _ := s.State(y); p != 9 {
		t.Fatalf("pts %d after the secret-chat rows went, 9 expected", p)
	}
}

func TestMemStore(t *testing.T) {
	m := NewMemStore()
	storeContract(t, m)
	now := time.Now()
	m.Now = func() time.Time { return now.Add(UpdateTTL + time.Minute) }
	if n, err := m.Expire(); err != nil || n != 2 {
		t.Fatalf("Expire after 7 days: %d rows %v, 2 expected", n, err)
	}
	if _, gone, _ := m.Since(0x9000000000100, 0, 10); gone != 4 {
		t.Fatalf("gone %d after the row at 4 expired", gone)
	}
}

func TestDBStore(t *testing.T) {
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	now := time.Now()
	s := &DBStore{DB: d, Now: func() time.Time { return now }}
	storeContract(t, s)
	now = now.Add(UpdateTTL + time.Minute)
	if n, err := s.Expire(); err != nil || n != 2 {
		t.Fatalf("Expire after 7 days: %d rows %v, 2 expected", n, err)
	}
	if _, gone, _ := s.Since(0x9000000000100, 0, 10); gone != 4 {
		t.Fatalf("gone %d after the row at 4 expired", gone)
	}
	chatStoreContract(t, &DBChatStore{DB: d})
}

func TestMemChatStore(t *testing.T) { chatStoreContract(t, NewMemChatStore()) }

func chatStoreContract(t *testing.T, cs ChatStore) {
	now := time.Unix(1_800_000_000, 0)
	if d, err := cs.Device(1); err != nil || d != nil {
		t.Fatalf("no device: %v %v", d, err)
	}
	if old, err := cs.SetDevice(ChatDevice{User: 1, Device: 10, Pubkey: []byte("k1"), AccountKey: []byte("a"), Signature: []byte("s"), Date: 5}); err != nil || old != nil {
		t.Fatalf("first SetDevice: %v %v", old, err)
	}
	old, err := cs.SetDevice(ChatDevice{User: 1, Device: 11, Pubkey: []byte("k2"), AccountKey: []byte("a"), Signature: []byte("s2"), Date: 6})
	if err != nil || old == nil || old.Device != 10 || string(old.Pubkey) != "k1" {
		t.Fatalf("second SetDevice: old %+v %v", old, err)
	}
	if d, _ := cs.Device(1); d == nil || d.Device != 11 || string(d.Signature) != "s2" || d.Date != 6 {
		t.Fatalf("Device after replace: %+v", d)
	}
	c := SecretChat{Date: 100, Admin: 1, AdminDevice: 11, Participant: 2, ParticipantDevice: 20, RandomID: -5, GA: []byte("ga"), GASig: []byte("sig")}
	a, made, err := cs.NewChat(c, now)
	if err != nil || !made || a.ID <= 0 || a.State != ChatRequested {
		t.Fatalf("NewChat: %+v %v %v", a, made, err)
	}
	again, made, _ := cs.NewChat(c, now)
	if made || again.ID != a.ID {
		t.Fatalf("repeated NewChat made %v id %d", made, again.ID)
	}
	other := c
	other.GA = []byte("ga2")
	b, made, _ := cs.NewChat(other, now)
	if !made || b.ID == a.ID {
		t.Fatalf("same random_id with another g_a: made %v id %d", made, b.ID)
	}
	if ok, err := cs.Accept(a.ID, []byte("gb"), []byte("gbs"), -42, now); err != nil || !ok {
		t.Fatalf("Accept: %v %v", ok, err)
	}
	if ok, _ := cs.Accept(a.ID, []byte("gb"), []byte("gbs"), -42, now); ok {
		t.Fatal("accepted twice")
	}
	got, _ := cs.Chat(a.ID)
	if got == nil || got.State != ChatAccepted || string(got.GB) != "gb" || got.Fingerprint != -42 || got.RandomID != -5 || string(got.GASig) != "sig" {
		t.Fatalf("Chat after accept: %+v", got)
	}
	if open, _ := cs.OpenChats(2, 1); len(open) != 2 {
		t.Fatalf("OpenChats(2, 1): %d", len(open))
	}
	if open, _ := cs.OpenChats(2, 3); len(open) != 0 {
		t.Fatalf("OpenChats(2, 3): %d", len(open))
	}
	if ok, _ := cs.Discard(b.ID, now); !ok {
		t.Fatal("Discard")
	}
	if ok, _ := cs.Discard(b.ID, now); ok {
		t.Fatal("discarded twice")
	}
	if ok, _ := cs.Accept(b.ID, nil, nil, 0, now); ok {
		t.Fatal("a discarded chat accepted")
	}
	if open, _ := cs.OpenChats(1, 0); len(open) != 1 || open[0].ID != a.ID {
		t.Fatalf("OpenChats(1) after the discard: %+v", open)
	}
	if c, _ := cs.Chat(9999); c != nil {
		t.Fatal("unknown chat found")
	}
	// Commitments: a repeat answers the first date; SentTo finds a message by its recipient only.
	r := SentRecord{Chat: a.ID, Sender: 1, Recipient: 2, RandomID: 77, Commitment: []byte("c32"), Date: 200}
	if d, fresh, err := cs.AddSent(r, now); err != nil || !fresh || d != 200 {
		t.Fatalf("AddSent: %d %v %v", d, fresh, err)
	}
	r2 := r
	r2.Date = 300
	if d, fresh, _ := cs.AddSent(r2, now); fresh || d != 200 {
		t.Fatalf("repeated AddSent: %d fresh %v", d, fresh)
	}
	if s, _ := cs.SentTo(a.ID, 77, 2); s == nil || s.Sender != 1 || string(s.Commitment) != "c32" {
		t.Fatalf("SentTo recipient: %+v", s)
	}
	if s, _ := cs.SentTo(a.ID, 77, 1); s != nil {
		t.Fatal("SentTo found the sender's own message as received")
	}
	cs.AddSent(SentRecord{Chat: a.ID, Sender: 2, Recipient: 1, RandomID: 78, Commitment: []byte("x"), Date: 201}, now.Add(time.Hour))
	if err := cs.DeleteSent(a.ID, 2, 78); err != nil {
		t.Fatal(err)
	}
	if s, _ := cs.SentTo(a.ID, 78, 1); s != nil {
		t.Fatal("a deleted record was found")
	}
	// Reports: one per reporter and message, newest first.
	if ok, err := cs.AddReport(Report{Created: now, Reporter: 2, Sender: 1, Chat: a.ID, RandomID: 77, Text: "bad", Reason: "spam"}); err != nil || !ok {
		t.Fatalf("AddReport: %v %v", ok, err)
	}
	if ok, _ := cs.AddReport(Report{Created: now, Reporter: 2, Sender: 1, Chat: a.ID, RandomID: 77, Text: "bad", Reason: "spam"}); ok {
		t.Fatal("reported twice")
	}
	cs.AddReport(Report{Created: now, Reporter: 2, Sender: 1, Chat: a.ID, RandomID: 79, Text: "worse", Reason: ""})
	if rs, _ := cs.Reports(10); len(rs) != 2 || rs[0].RandomID != 79 || rs[1].Text != "bad" || rs[1].Reason != "spam" || rs[1].Reporter != 2 {
		t.Fatalf("Reports: %+v", rs)
	}
	// Sweep after 30 days: the commitment and the discarded chat go, the accepted chat and the reports stay.
	sent, chats, err := cs.Sweep(now.Add(SentTTL + time.Minute))
	if err != nil || sent != 1 || chats != 1 {
		t.Fatalf("Sweep: %d commitments %d chats %v", sent, chats, err)
	}
	if s, _ := cs.SentTo(a.ID, 77, 2); s != nil {
		t.Fatal("commitment kept past 30 days")
	}
	if c, _ := cs.Chat(b.ID); c != nil {
		t.Fatal("discarded chat kept past 30 days")
	}
	if c, _ := cs.Chat(a.ID); c == nil {
		t.Fatal("accepted chat swept")
	}
	if rs, _ := cs.Reports(10); len(rs) != 2 {
		t.Fatal("reports swept")
	}
}
