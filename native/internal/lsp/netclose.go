package lsp

import (
	"fmt"
	"strconv"
	"strings"

	"fh2mp/internal/diag"
)

// netClose is one close of a game's connection to the game server, as the shim's net watch (shim/src/net_watch.inc) reports it on the game's
// next request, once, in the X-FH2MP-Net header: "v1 seq=<n> reason=<8 hex>", plus " msg=<id> result=<8 hex> bits=<at>/<size>" when the
// close followed a message the game refused. FH2 1.0.0.64 closes an established connection when one message does not decode
// (docs/re/netv2.md section 3), and that close may send the game server nothing, so this is the server side's only sight of it.
type netClose struct {
	Seq            uint64
	Reason, Result uint32
	Msg            int // -1: no refused message
	At, Size       uint64
}

// parseNetClose reads the header value; anything else than the shape above is refused (it is a client's text).
func parseNetClose(v string) (netClose, bool) {
	nc := netClose{Msg: -1}
	if len(v) > 120 {
		return nc, false
	}
	f := strings.Split(v, " ")
	if (len(f) != 3 && len(f) != 6) || f[0] != "v1" {
		return nc, false
	}
	val := func(field, key string) (string, bool) {
		s, ok := strings.CutPrefix(field, key+"=")
		return s, ok && s != ""
	}
	hex8 := func(s string) (uint32, bool) {
		if len(s) != 8 {
			return 0, false
		}
		x, err := strconv.ParseUint(s, 16, 32)
		return uint32(x), err == nil && strings.ToLower(s) == s
	}
	dec := func(s string, max uint64) (uint64, bool) {
		x, err := strconv.ParseUint(s, 10, 64)
		return x, err == nil && x <= max && strconv.FormatUint(x, 10) == s
	}
	s, ok := val(f[1], "seq")
	if !ok {
		return nc, false
	}
	if nc.Seq, ok = dec(s, 1<<32); !ok {
		return nc, false
	}
	if s, ok = val(f[2], "reason"); !ok {
		return nc, false
	}
	if nc.Reason, ok = hex8(s); !ok {
		return nc, false
	}
	if len(f) == 3 {
		return nc, true
	}
	if s, ok = val(f[3], "msg"); !ok {
		return nc, false
	}
	m, ok := dec(s, 127) // a NetV2 message id is 7 bits
	if !ok {
		return nc, false
	}
	nc.Msg = int(m)
	if s, ok = val(f[4], "result"); !ok {
		return nc, false
	}
	if nc.Result, ok = hex8(s); !ok {
		return nc, false
	}
	if s, ok = val(f[5], "bits"); !ok {
		return nc, false
	}
	at, size, cut := strings.Cut(s, "/")
	if !cut {
		return nc, false
	}
	if nc.At, ok = dec(at, 1<<32); !ok {
		return nc, false
	}
	if nc.Size, ok = dec(size, 1<<32); !ok {
		return nc, false
	}
	return nc, true
}

// netCloseLine is the log line for one reported close; game is the run id without its request number.
func netCloseLine(game string, nc netClose) string {
	if game == "" {
		game = "unknown"
	}
	line := fmt.Sprintf("  netwatch: game run %s closed its connection to the game server (close #%d of that game): reason %#x", game, nc.Seq, nc.Reason)
	if nc.Msg >= 0 {
		line += fmt.Sprintf(", after it refused message %d (result %#x) starting at bit %d of a %d-bit datagram; the game's fh2mp.log has the bytes",
			nc.Msg, nc.Result, nc.At, nc.Size)
	}
	return line
}

// noteNetClose logs a game's X-FH2MP-Net header: a close after a refused message as WARN (a player lost the session to a message), any
// other close as INFO, at most one line per game per 10 s (a real game closes far less often; a resent header is not logged twice); a header
// that is not the shim's shape once per 5 minutes per game.
func (c *Core) noteNetClose(r *Req, v, from string) {
	game, _, _ := strings.Cut(r.Run, ".")
	key := game
	if key == "" {
		key = "addr:" + from // a game without the launcher's session has no run id: one key per address keeps such games apart
	}
	nc, ok := parseNetClose(v)
	if !ok {
		c.LogEvery(r, "netclose-bad/"+key, 300, fmt.Sprintf("  netwatch: X-FH2MP-Net header not understood, ignored: %q", cut(v, 120)), diag.Warn, "")
		return
	}
	level := diag.Info
	if nc.Msg >= 0 {
		level = diag.Warn
	}
	c.LogEvery(r, "netclose/"+key, 10, netCloseLine(game, nc), level, "")
}
