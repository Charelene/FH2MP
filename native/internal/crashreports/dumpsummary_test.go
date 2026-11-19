package crashreports

import (
	"encoding/binary"
	"strings"
	"testing"
	"unicode/utf16"

	"fh2mp/internal/pyjson"
)

// tinyDump is a minidump (minidumpapiset.h layouts) with one module, a 0x40-byte stack and an access violation reading 0x68 at module+0x2d8ea4.
func tinyDump(path string) []byte {
	var b []byte
	u32 := func(v uint32) { b = binary.LittleEndian.AppendUint32(b, v) }
	u64 := func(v uint64) { b = binary.LittleEndian.AppendUint64(b, v) }
	b = append(b, "MDMP"...)
	u32(0xa793)
	u32(3)
	u32(32)
	b = append(b, make([]byte, 16+3*12)...)
	dir := func(i int, typ, rva uint32) {
		binary.LittleEndian.PutUint32(b[32+12*i:], typ)
		binary.LittleEndian.PutUint32(b[32+12*i+8:], rva)
	}
	const base = 0x7ff612340000
	name := uint32(len(b))
	w := utf16.Encode([]rune(path))
	u32(uint32(2 * len(w)))
	for _, c := range w {
		b = binary.LittleEndian.AppendUint16(b, c)
	}
	mods := uint32(len(b))
	u32(1)
	u64(base)
	u32(0x400000)
	u32(0)
	u32(0)
	u32(name)
	b = append(b, make([]byte, 108-24)...)
	stack := make([]byte, 0x40)
	binary.LittleEndian.PutUint64(stack[0x8:], base+0x2d8f10)
	stackRva := uint32(len(b))
	b = append(b, stack...)
	ctx := make([]byte, 0x4d0)
	binary.LittleEndian.PutUint64(ctx[0x98:], 0x5f000) // rsp
	ctxRva := uint32(len(b))
	b = append(b, ctx...)
	threads := uint32(len(b))
	u32(1)
	u32(77)
	b = append(b, make([]byte, 20)...)
	u64(0x5f000)
	u32(uint32(len(stack)))
	u32(stackRva)
	u32(uint32(len(ctx)))
	u32(ctxRva)
	exc := uint32(len(b))
	u32(77)
	u32(0)
	u32(0xc0000005)
	u32(0)
	u64(0)
	u64(base + 0x2d8ea4)
	u32(2)
	u32(0)
	u64(0)
	u64(0x68)
	b = append(b, make([]byte, 13*8)...)
	u32(uint32(len(ctx)))
	u32(ctxRva)
	dir(0, 4, mods)
	dir(1, 3, threads)
	dir(2, 6, exc)
	return b
}

func TestDumpSummaryNamesTheFaultAndTheStack(t *testing.T) {
	got := DumpSummary(tinyDump(`C:\Users\someone\Games\Azimuth\azimuth.exe`))
	want := "**Dump, read on arrival:** `0xc0000005` access violation at `azimuth+0x2d8ea4`, reading `0x68`, thread 77.\n" +
		"Stack values that point into a module, from rsp up: `azimuth+0x2d8f10`."
	if got != want {
		t.Fatalf("got  %q\nwant %q", got, want)
	}
	if strings.Contains(got, "someone") {
		t.Fatal("the module's folder (the player's user name) reached the text")
	}
}

func TestDumpSummaryIsEmptyWithoutAReadableDump(t *testing.T) {
	for _, d := range [][]byte{nil, []byte("MDMP"), []byte("not a dump, but long enough to look at")} {
		if s := DumpSummary(d); s != "" {
			t.Fatalf("%q -> %q", d, s)
		}
	}
}

func TestIssueAndCommentCarryTheDumpSummary(t *testing.T) {
	r := &Report{Dump: tinyDump("FH2MP.dll"), HasDump: true}
	issue := IssueBody("0123456789abcdef", r, "", LinkBase)
	if !strings.Contains(issue, "access violation at `FH2MP.dll+0x2d8ea4`") || strings.Index(issue, "**Dump") < strings.Index(issue, "| Report id |") {
		t.Fatalf("issue:\n%s", issue)
	}
	comment := CommentBody("0123456789abcdef", r, LinkBase)
	if !strings.Contains(comment, "access violation at `FH2MP.dll+0x2d8ea4`") || strings.Index(comment, "**Dump") > strings.Index(comment, "crash.dmp") {
		t.Fatalf("comment:\n%s", comment)
	}
}

// Without Azimuth's crash.dmp the Mod Platform's attached minidump is summarised.
func TestTheAttachedDumpIsUsedWithoutCrashDmp(t *testing.T) {
	r := &Report{Attachments: pyjson.Obj{{K: "notes.txt", V: []byte("x")}, {K: "mod-crash-1234.dmp", V: tinyDump("game.exe")}}}
	if s := DumpSummary(reportDump(r)); !strings.Contains(s, "`game.exe+0x2d8ea4`") {
		t.Fatalf("got %q", s)
	}
}

// Reports without a failure line used to open one issue each; with a dump they group by its fault, per game, and a failure line still wins.
func TestReportsWithoutAFailureLineGroupByTheirDump(t *testing.T) {
	a := &Report{Dump: tinyDump(`C:\a\azimuth.exe`), HasDump: true}
	b := &Report{Dump: tinyDump(`D:\other\folder\azimuth.exe`), HasDump: true}
	ka, title := IssueKey(a, "")
	kb, _ := IssueKey(b, "")
	if ka != "dump:c0000005@azimuth+0x2d8ea4" || ka != kb || title != "Crash 0xC0000005 at azimuth+0x2d8ea4 (read from the dump)" {
		t.Fatalf("keys %q %q, title %q", ka, kb, title)
	}
	fh3 := &Report{Dump: a.Dump, HasDump: true, Fields: pyjson.Obj{{K: "game", V: "Forza Horizon 3"}}}
	if k, t3 := IssueKey(fh3, ""); k != "forza horizon 3|"+ka || !strings.HasPrefix(t3, "Forza Horizon 3: Crash 0xC0000005") {
		t.Fatalf("fh3 key %q title %q", k, t3)
	}
	line := "[+12.3] CRASHED exception 0xc0000005 at game+0x2d8ea4"
	want, _ := Signature(line)
	if k, _ := IssueKey(a, line); k != want || k == ka {
		t.Fatalf("with a failure line the key is %q, want %q", k, want)
	}
	if k, _ := IssueKey(&Report{}, ""); k != "" {
		t.Fatalf("no line and no dump: %q", k)
	}
}
