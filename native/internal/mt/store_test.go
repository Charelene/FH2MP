package mt

import (
	"testing"
	"time"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
)

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
	rows, first, err := s.Since(x, 0, 2)
	if err != nil || len(rows) != 2 || first != 1 || rows[1][0] != 2 {
		t.Fatalf("Since(0, 2): %v first %d %v", rows, first, err)
	}
	if err := s.Ack(x, 2); err != nil {
		t.Fatal(err)
	}
	rows, first, _ = s.Since(x, 0, 10)
	if len(rows) != 1 || first != 3 || rows[0][0] != 3 {
		t.Fatalf("Since(0) after Ack(2): %v first %d", rows, first)
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
	rows, first, _ = s.Since(x, 3, 10)
	if len(rows) != 1 || first != 4 {
		t.Fatalf("Since(3) after the 4th: %v first %d", rows, first)
	}
}

func TestMemStore(t *testing.T) { storeContract(t, NewMemStore()) }

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
}
