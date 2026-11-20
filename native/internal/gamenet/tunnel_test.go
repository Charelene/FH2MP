package gamenet

import (
	"encoding/binary"
	"io"
	"net"
	"testing"
	"time"

	"fh2mp/internal/bits"
	"fh2mp/internal/game"
	"fh2mp/internal/netv2"
)

func freePort(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

func frame(b []byte) []byte {
	out := make([]byte, 2+len(b))
	binary.BigEndian.PutUint16(out, uint16(len(b)))
	copy(out[2:], b)
	return out
}

func openPacket() []byte {
	return netv2.BuildPacket([]netv2.Msg{func(w *bits.Writer) {
		w.Bits(netv2.MsgOpenConnection, 7)
		w.String(netv2.VersionString)
		w.String(netv2.GameString)
		w.Bits(0, 2)
		w.Bits(0x9000000000777, 64)
		w.Bits(2, 32)
		w.Bits(0, 32)
		w.Bits(7, 32)
	}}, 0xFFFF, 0, 0)
}

func readFrame(t *testing.T, c net.Conn) []byte {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(3 * time.Second))
	var hdr [2]byte
	if _, err := io.ReadFull(c, hdr[:]); err != nil {
		t.Fatalf("no frame: %v", err)
	}
	b := make([]byte, binary.BigEndian.Uint16(hdr[:]))
	if _, err := io.ReadFull(c, b); err != nil {
		t.Fatal(err)
	}
	return b
}

func closed(c net.Conn, within time.Duration) bool {
	c.SetReadDeadline(time.Now().Add(within))
	buf := make([]byte, 16)
	for {
		if _, err := c.Read(buf); err != nil {
			ne, ok := err.(net.Error)
			return !(ok && ne.Timeout())
		}
	}
}

func startServer(t *testing.T) (*Server, string) {
	t.Helper()
	tp := freePort(t)
	t.Setenv("FH2MP_TUNNEL_PORT", itoa(tp))
	t.Setenv("FH2MP_CONTROL_PORT", "0")
	t.Setenv("FH2MP_UDP_AUTH", "off")
	s, err := New(Config{Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() { s.Run(); close(done) }()
	t.Cleanup(func() {
		s.events <- event{kind: 5}
		<-done
		s.udp.Close()
		s.tl.Close()
	})
	return s, "127.0.0.1:" + itoa(tp)
}

func itoa(n int) string {
	return string(append([]byte(nil), []byte(func() string {
		if n == 0 {
			return "0"
		}
		var d []byte
		for n > 0 {
			d = append([]byte{byte('0' + n%10)}, d...)
			n /= 10
		}
		return string(d)
	}())...))
}

func TestTunnelCarriesTheHandshake(t *testing.T) {
	_, addr := startServer(t)
	c, err := net.Dial("tcp4", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.Write(frame(openPacket()))
	reply := readFrame(t, c)
	r := bits.NewReader(reply)
	h, err := netv2.ParseHeader(r)
	if err != nil || h.Count != 1 {
		t.Fatalf("header %+v err %v", h, err)
	}
	if typ, _ := r.Bits(7); typ != netv2.MsgOpenConnectionResponse {
		t.Fatalf("first message type %d, want OpenConnectionResponse", typ)
	}
	c.Write(frame(nil)) // a keep-alive frame is not a datagram
	c.Write(frame(netv2.BuildPacket([]netv2.Msg{netv2.Ping(5)}, 0xFFFF, 0, 0)))
	pong := readFrame(t, c)
	r = bits.NewReader(pong)
	netv2.ParseHeader(r)
	if typ, _ := r.Bits(7); typ != netv2.MsgPong {
		t.Fatalf("answer to Ping has type %d, want Pong", typ)
	}
}

func TestTunnelRefusesBadFramesAndTooManyConnections(t *testing.T) {
	_, addr := startServer(t)
	bad, _ := net.Dial("tcp4", addr)
	defer bad.Close()
	bad.Write([]byte{0x10, 0x00}) // announces 4096 bytes, the limit is 2048
	if !closed(bad, 2*time.Second) {
		t.Fatal("an oversize frame must close the connection")
	}
	var open []net.Conn
	for i := 0; i < perIP; i++ {
		c, err := net.Dial("tcp4", addr)
		if err != nil {
			t.Fatal(err)
		}
		open = append(open, c)
	}
	time.Sleep(200 * time.Millisecond)
	extra, _ := net.Dial("tcp4", addr)
	defer extra.Close()
	if !closed(extra, 2*time.Second) {
		t.Fatalf("a %dth connection from one address must be refused", perIP+1)
	}
	for _, c := range open {
		c.Close()
	}
}

func TestTunnelDropsConnectionsThatNeverOpenAGame(t *testing.T) {
	old := helloSeconds
	helloSeconds = 0.4
	defer func() { helloSeconds = old }()
	s, addr := startServer(t)
	_ = s
	c, _ := net.Dial("tcp4", addr)
	defer c.Close()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if closed(c, 300*time.Millisecond) {
			return
		}
	}
	t.Fatal("a connection that opens no game connection must be closed after the hello time")
}

// With the TCP fallback the shim's horn socket is a connection of its own that never opens a game; once the hub has matched a signed side-channel
// request on it (KeepSide), it stays past the hello time. Before 2026-11-19 it was closed and reopened every 10 s (42,149 times on prod in 6 days).
func TestTunnelKeepsAConnectionCarryingSideChannelTraffic(t *testing.T) {
	old := helloSeconds
	helloSeconds = 0.4
	defer func() { helloSeconds = old }()
	s, addr := startServer(t)
	side, err := net.Dial("tcp4", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer side.Close()
	other, err := net.Dial("tcp4", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer other.Close()
	key := game.Addr{IP: "127.0.0.1", Port: side.LocalAddr().(*net.TCPAddr).Port}
	deadline := time.Now().Add(3 * time.Second)
	for !s.tun.has(key) && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if !s.tun.has(key) {
		t.Fatal("the connection was never registered")
	}
	s.tun.KeepSide(key)
	if !closed(other, 3*time.Second) {
		t.Fatal("a connection with neither a game nor side-channel traffic must still be closed after the hello time")
	}
	if closed(side, 1500*time.Millisecond) {
		t.Fatal("a connection carrying signed side-channel traffic was closed after the hello time")
	}
}
