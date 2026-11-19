package lsp

import (
	"strings"
	"testing"

	"fh2mp/internal/diag"
)

// The header values are the ones shim/tests/test_guard_counts.sh expects guard_counts_text to write.
func TestGuardCountsHeader(t *testing.T) {
	got, ok := parseGuardCounts("v1 camanim=3 dtconv=1 travelxp=1234567")
	if !ok || len(got) != 3 || got[0] != (guardCount{"camanim", 3}) || got[2] != (guardCount{"travelxp", 1234567}) {
		t.Fatalf("got %+v %v", got, ok)
	}
	for _, v := range []string{"", "v1", "v2 camanim=1", "v1 camanim=0", "v1 Camanim=1", "v1 camanim=01", "v1 cam anim=1", "v1 camanim=-1",
		"v1 camanim=1\nforged", "v1 camanim=1 " + strings.Repeat("x=1 ", 40)} {
		if g, ok := parseGuardCounts(v); ok {
			t.Errorf("%q read as %+v", v, g)
		}
	}
}

func TestGuardCountsLogLine(t *testing.T) {
	var lines []string
	c := &Core{Out: func(l string) { lines = append(lines, l) }, rate: diag.NewRateLimit(nil, 0), Verbosity: diag.Verbosity{Base: diag.Info}}
	c.noteGuardCounts(&Req{Run: "ab12cd34.5"}, "v1 camanim=3 dtconv=1")
	c.noteGuardCounts(&Req{Run: "ab12cd34.6"}, "v1 camanim=4 dtconv=1") // within the minute: counted, not written
	if len(lines) != 1 || !strings.Contains(lines[0], "guards: game run ab12cd34: crashes turned away so far by camanim 3, dtconv 1") || strings.Contains(lines[0], "WARN") {
		t.Fatalf("lines %q", lines)
	}
}
