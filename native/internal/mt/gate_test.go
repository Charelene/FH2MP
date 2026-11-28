package mt

import (
	"errors"
	"net"
	"os"
	"sync/atomic"
	"testing"
	"time"
)

// The operator's mt_chat switch (internal/flags) is the server's Gate: off, a new connection is closed at once; on again, it is taken.
func TestAConnectionIsClosedWhileTheGateIsOff(t *testing.T) {
	clk := &fakeClock{}
	s, err := New(Config{Realm: "test", Now: clk.now, IdleTimeout: 20 * time.Second, HandshakeWait: 20 * time.Second},
		serverKeyForTest(t), newFakeAccounts("test"), NewMemStore())
	if err != nil {
		t.Fatal(err)
	}
	var open atomic.Bool
	s.Gate = open.Load
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go func() { _ = s.Serve(l) }()
	t.Cleanup(func() { _ = s.Close() })
	read := func(wait time.Duration) error {
		nc, err := net.DialTimeout("tcp", l.Addr().String(), 3*time.Second)
		if err != nil {
			t.Fatal(err)
		}
		defer nc.Close()
		_ = nc.SetReadDeadline(time.Now().Add(wait))
		_, err = nc.Read(make([]byte, 1))
		return err
	}
	if err := read(3 * time.Second); err == nil || errors.Is(err, os.ErrDeadlineExceeded) {
		t.Fatalf("gate off: the connection was kept open (%v), want it closed", err)
	}
	open.Store(true)
	if err := read(300 * time.Millisecond); !errors.Is(err, os.ErrDeadlineExceeded) {
		t.Fatalf("gate on: read gave %v, want the server to wait for the client's header", err)
	}
}
