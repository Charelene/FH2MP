package game

import (
	"math"
	"strings"
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/netv2"
	"fh2mp/internal/pyrand"
	"github.com/Charelene/encore-paddock/bits"
)

// BenchmarkCarPredictionRelay: one player's car prediction relayed in a Free Roam world of 12 connected players (no network: a fake socket),
// the path every car takes 10-30 times a second.
func BenchmarkCarPredictionRelay(b *testing.B) {
	b.Setenv("FH2MP_UDP_AUTH", "off")
	clk := &fakeClock{t: 1000}
	cat, err := catalog.Load()
	if err != nil {
		b.Skip("catalog not available:", err)
	}
	out := NewLogOut(func(chunk string) error { _ = strings.TrimSpace(chunk); return nil })
	w, err := NewWorld("[bench]", out, clk, pyrand.New(1), &fakeSock{clk: clk}, nil, &Sinks{}, func() *catalog.Catalog { return cat }, false, false, 0)
	if err != nil {
		b.Fatal(err)
	}
	var peers []*Peer
	for i := 0; i < 12; i++ {
		p := NewPeer(Addr{"127.0.0.1", 40000 + i}, 1000)
		p.Owner, p.Xuid, p.Connected, p.ConnectedAt = i+1, uint64(100+i), true, 900
		p.State, p.OpenedAt, p.Entered = PeerConnected, 900, true
		w.Peers.Set(p.Addr, p)
		peers = append(peers, p)
	}
	datagram := func(stamp uint64) []byte {
		body := bits.FromUint64(uint64(math.Float32bits(100))).
			Or(bits.FromUint64(uint64(math.Float32bits(50))).Lsh(64)).
			Or(bits.FromUint64(stamp & 0xFFFFFFFF).Lsh(netv2.CarPredictionBodyBits - 32))
		return netv2.BuildPacket([]netv2.Msg{func(wr *bits.Writer) {
			wr.Bits(netv2.MsgCarPredictionFull, 7)
			wr.Bits(0, 1)
			wr.Bits(0, 1)
			wr.Bits(5, netv2.CarKeyIDBits)
			wr.Raw(body, netv2.CarPredictionBodyBits)
		}}, netv2.UnreliableID, 0, 0)
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		clk.t += 0.033
		for _, p := range peers {
			p.LastRx, p.LastKeepalive, p.LastTimeSync = clk.t, clk.t, clk.t
		}
		w.Handle(datagram(uint64(1000+i*33)), peers[0].Addr)
		w.Flush(clk.t + 1) // the server loop flushes every due outbox after a datagram
		if i%3 == 0 {
			w.Tick()
		}
	}
}
