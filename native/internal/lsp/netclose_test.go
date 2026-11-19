package lsp

import (
	"strings"
	"testing"

	"fh2mp/internal/diag"
)

// The header values are the ones shim/tests/test_net_watch.sh expects net_watch_header_text to write.
func TestNetCloseHeaderWithARefusedMessage(t *testing.T) {
	nc, ok := parseNetClose("v1 seq=3 reason=80600006 msg=32 result=80600006 bits=118/600")
	want := netClose{Seq: 3, Reason: 0x80600006, Msg: 32, Result: 0x80600006, At: 118, Size: 600}
	if !ok || nc != want {
		t.Fatalf("got %+v %v, want %+v", nc, ok, want)
	}
}

func TestNetCloseHeaderWithoutAMessage(t *testing.T) {
	nc, ok := parseNetClose("v1 seq=1 reason=80600008")
	if !ok || nc != (netClose{Seq: 1, Reason: 0x80600008, Msg: -1}) {
		t.Fatalf("got %+v %v", nc, ok)
	}
}

func TestNetCloseHeaderRefusesOtherText(t *testing.T) {
	for _, v := range []string{
		"",
		"v2 seq=1 reason=80600008",
		"v1 seq=1 reason=8060008",   // 7 digits
		"v1 seq=1 reason=8060000A",  // the shim writes lower case
		"v1 seq=1 reason=80600008 ", // a trailing field
		"v1  seq=1 reason=80600008", // two spaces
		"v1 seq=01 reason=80600008", // not how the shim writes a number
		"v1 seq=-1 reason=80600008", // no sign
		"v1 reason=80600008 seq=1",  // order
		"v1 seq=1 reason=80600006 msg=128 result=80600006 bits=1/2",                          // a message id is 7 bits
		"v1 seq=1 reason=80600006 msg=32 result=80600006 bits=12",                            // no size
		"v1 seq=1 reason=80600006 msg=32 result=80600006",                                    // 5 fields
		"v1 seq=1 reason=80600008\n12:00:00 forged line",                                     // a second log line
		"v1 seq=1 reason=80600006 msg=32 result=80600006 bits=1/2" + strings.Repeat("0", 80), // too long
	} {
		if nc, ok := parseNetClose(v); ok {
			t.Errorf("%q read as %+v", v, nc)
		}
	}
}

func TestNetCloseLogLines(t *testing.T) {
	var lines []string
	c := &Core{Out: func(l string) { lines = append(lines, l) }, rate: diag.NewRateLimit(nil, 0), Verbosity: diag.Verbosity{Base: diag.Info}}
	r := &Req{Run: "ab12cd34.7"}
	c.noteNetClose(r, "v1 seq=1 reason=80600006 msg=32 result=80600006 bits=88/600", "192.0.2.1")
	c.noteNetClose(&Req{Run: "ab12cd34.8"}, "v1 seq=1 reason=80600006 msg=32 result=80600006 bits=88/600", "192.0.2.1") // the same close again
	if len(lines) != 1 {
		t.Fatalf("lines %q", lines)
	}
	for _, part := range []string{"WARN ", "netwatch: game run ab12cd34 closed its connection to the game server (close #1 of that game): reason 0x80600006",
		"after it refused message 32 (result 0x80600006) starting at bit 88 of a 600-bit datagram", "run=ab12cd34.7"} {
		if !strings.Contains(lines[0], part) {
			t.Errorf("line %q lacks %q", lines[0], part)
		}
	}
	lines = nil
	c.noteNetClose(&Req{Run: "ffff0000.2"}, "v1 seq=2 reason=80600008", "192.0.2.1")
	if len(lines) != 1 || strings.Contains(lines[0], "WARN") || !strings.Contains(lines[0], "game run ffff0000 closed its connection to the game server (close #2 of that game): reason 0x80600008") ||
		strings.Contains(lines[0], "refused") {
		t.Fatalf("lines %q", lines)
	}
	lines = nil
	c.noteNetClose(&Req{Run: "ffff0000.3"}, "v1 seq=3 reason=80600008\r\nforged", "192.0.2.1")
	if len(lines) != 1 || !strings.Contains(lines[0], `X-FH2MP-Net header not understood, ignored: "v1 seq=3 reason=80600008\r\nforged"`) {
		t.Fatalf("lines %q", lines)
	}
}
