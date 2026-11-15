package game

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/pyrand"
)

type trustRig struct {
	t   *testing.T
	clk *fakeClock
	dir string
	hub *Hub
	log []string
}

// newTrustRig opens a hub whose cheaters.json and trusted.json live in dir.
func newTrustRig(t *testing.T, dir string) *trustRig {
	t.Helper()
	t.Setenv("FH2MP_UDP_AUTH", "off")
	r := &trustRig{t: t, clk: &fakeClock{t: 1000}, dir: dir}
	cat, err := catalog.Load()
	if err != nil {
		t.Skip("catalog not available:", err)
	}
	out := NewLogOut(func(chunk string) error {
		r.log = append(r.log, strings.Split(strings.TrimRight(chunk, "\n"), "\n")...)
		return nil
	})
	hub, err := NewHub(HubConfig{Clock: r.clk, Rng: pyrand.New(1), Sock: &fakeSock{clk: r.clk}, Out: out, Catalog: cat, CtlSend: func([]byte, Addr) {},
		Sinks: &Sinks{}, LogPath: filepath.Join(dir, "server.log")})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if hub.Events != nil {
			hub.Events.Close()
		}
	})
	r.hub = hub
	return r
}

func (r *trustRig) has(sub string) bool {
	for _, l := range r.log {
		if strings.Contains(l, sub) {
			return true
		}
	}
	return false
}

// trust takes a pooled player out of the pool, keeps them out of it, survives a restart; untrust puts the old rules back.
func TestTrustControlAndPool(t *testing.T) {
	dir := t.TempDir()
	r := newTrustRig(t, dir)
	const x = 0x90000b675a34f
	r.hub.Flag(x, "speed 453 km/h")
	if !r.hub.Flagged.Has(x) {
		t.Fatal("Flag did not pool an untrusted player")
	}
	r.hub.ControlText("trust 90000b675a34f", nil)
	if r.hub.Flagged.Has(x) || !r.hub.IsTrusted(x) {
		t.Fatalf("after trust: pooled %v trusted %v", r.hub.Flagged.Has(x), r.hub.IsTrusted(x))
	}
	if !r.has("taken out of the cheater pool") {
		t.Errorf("no trust line: %q", r.log)
	}
	r.hub.Flag(x, "speed 470 km/h")
	if r.hub.Flagged.Has(x) || !r.has("not put in the cheater pool (speed 470 km/h): the account is trusted") {
		t.Errorf("a trusted player was pooled or the refusal not logged: %q", r.log)
	}
	if out := jsonDumps(r.hub.trustStatus()); !strings.Contains(out, `"xuid": "90000b675a34f"`) || !strings.Contains(out, `"lobbies": []`) {
		t.Errorf("trusted status %s", out)
	}
	r.hub.ControlText("trust 0", nil)
	r.hub.ControlText("trust zz", nil)
	if len(r.hub.Trusted) != 1 {
		t.Errorf("bad trust messages changed the list: %v", r.hub.Trusted)
	}

	again := newTrustRig(t, dir) // a restart reads trusted.json back
	if !again.hub.IsTrusted(x) || again.hub.TrustError != "" {
		t.Fatalf("after restart: trusted %v error %q", again.hub.IsTrusted(x), again.hub.TrustError)
	}
	again.hub.ControlText("untrust 90000b675a34f", nil)
	if again.hub.IsTrusted(x) {
		t.Fatal("untrust left the player trusted")
	}
	again.hub.Flag(x, "speed 470 km/h")
	if !again.hub.Flagged.Has(x) {
		t.Error("an untrusted player was not pooled")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "trusted.json")); string(b) != "{}" {
		t.Errorf("trusted.json after untrust: %q", b)
	}
}

// A broken trusted.json is reported at start-up and trusts nobody.
func TestTrustFileBroken(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "trusted.json"), []byte(`{"abc": 1}`), 0o644); err != nil {
		t.Fatal(err)
	}
	r := newTrustRig(t, dir)
	if r.hub.TrustError == "" || len(r.hub.Trusted) != 0 || !r.has("trusted players file") {
		t.Errorf("error %q trusted %v log %q", r.hub.TrustError, r.hub.Trusted, r.log)
	}
}

// The speed and flight checks still log a trusted player but do not end the connection; anyone else is kicked.
func TestTrustedNotKickedForSpeedOrFlight(t *testing.T) {
	for _, c := range []struct {
		name    string
		trusted bool
		climb   float64 // metres per second up
		line    string
	}{
		{"speed, untrusted", false, 0, "SPEED:"},
		{"speed, trusted", true, 0, "SPEED:"},
		{"flight, untrusted", false, 60, "FLIGHT:"},
		{"flight, trusted", true, 60, "FLIGHT:"},
	} {
		r := newLoadRig(t, 1)
		hub := &Hub{Trusted: map[uint64]float64{}}
		r.w.Hub = hub
		p := r.peers[0]
		if c.trusted {
			hub.Trusted[p.Xuid] = 1
		}
		mid := int64(1234)
		p.ModelID = &mid
		x, y := 0.0, 0.0
		for i := 0; i < 6; i++ {
			r.clk.t = 1000 + float64(i)
			r.w.TrackSpeed(p, [3]float64{x, y, 0}, nil, nil)
			if c.climb > 0 {
				y += c.climb
				x += 10
			} else {
				x += 130 // 468 km/h, over the hard cap
			}
		}
		var got string
		for _, l := range r.log {
			if strings.Contains(l, c.line) {
				got = l
			}
		}
		if got == "" {
			t.Errorf("%s: no %s line: %q", c.name, c.line, r.log)
			continue
		}
		kicked := p.KickReason != nil
		if kicked == c.trusted {
			t.Errorf("%s: kicked %v", c.name, kicked)
		}
		wantTail := ": ending the connection"
		if c.trusted {
			wantTail = ": trusted account, not kicked"
		}
		if !strings.HasSuffix(got, wantTail) {
			t.Errorf("%s: line %q does not end in %q", c.name, got, wantTail)
		}
	}
}
