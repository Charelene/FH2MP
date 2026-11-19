package lsp

import (
	"fmt"
	"strconv"
	"strings"

	"fh2mp/internal/diag"
)

// guardCount is one crash guard's count as the shim reports it (shim/src/guard_counts.inc): how often that guard turned a crash away in the game
// so far. The X-FH2MP-Guards header comes on the game's next request after a count changed: "v1 camanim=3 dtconv=1".
type guardCount struct {
	Name  string
	Count uint64
}

// parseGuardCounts reads the header strictly (it is a client's text): "v1", then name=count pairs, names in a-z, at most 32 of them.
func parseGuardCounts(v string) ([]guardCount, bool) {
	if len(v) > 400 {
		return nil, false
	}
	f := strings.Split(v, " ")
	if len(f) < 2 || len(f) > 33 || f[0] != "v1" {
		return nil, false
	}
	var out []guardCount
	for _, p := range f[1:] {
		name, num, ok := strings.Cut(p, "=")
		if !ok || name == "" || len(name) > 20 || strings.Trim(name, "abcdefghijklmnopqrstuvwxyz") != "" {
			return nil, false
		}
		n, err := strconv.ParseUint(num, 10, 32)
		if err != nil || n == 0 || strconv.FormatUint(n, 10) != num {
			return nil, false
		}
		out = append(out, guardCount{name, n})
	}
	return out, true
}

// noteGuardCounts logs a game's guard counts, at most once a minute per game (counts only grow, and the next change carries them all again).
func (c *Core) noteGuardCounts(r *Req, v string) {
	game, _, _ := strings.Cut(r.Run, ".")
	counts, ok := parseGuardCounts(v)
	if !ok {
		c.LogEvery(r, "guards-bad/"+game, 300, fmt.Sprintf("  guards: X-FH2MP-Guards header not understood, ignored: %q", cut(v, 120)), diag.Warn, "")
		return
	}
	if game == "" {
		game = "unknown"
	}
	parts := make([]string, len(counts))
	for i, g := range counts {
		parts[i] = fmt.Sprintf("%s %d", g.Name, g.Count)
	}
	c.LogEvery(r, "guards/"+game, 60, fmt.Sprintf("  guards: game run %s: crashes turned away so far by %s", game, strings.Join(parts, ", ")), diag.Info, "")
}
