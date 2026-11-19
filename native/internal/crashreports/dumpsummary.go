package crashreports

import (
	"fmt"
	"strings"

	"fh2mp/internal/minidump"
)

// exceptionNames are the Windows exception codes a game crash comes with (ntstatus.h; 0xe06d7363 is the MSVC C++ throw).
var exceptionNames = map[uint32]string{
	0xc0000005: "access violation",
	0xc0000094: "integer divide by zero",
	0xc00000fd: "stack overflow",
	0xc000001d: "illegal instruction",
	0xc0000409: "fast fail",
	0xc0000374: "heap corruption",
	0x80000003: "breakpoint",
	0xe06d7363: "C++ exception",
}

// DumpSummary is what an issue says about the report's minidump, read on arrival (native/internal/minidump): the exception, where it happened
// as module+offset, and the first stack values above rsp that point into a module (the return addresses a triage starts from). Module names
// carry no folder. "" when the dump cannot be read or has no exception record, so a report without one reads as before.
func DumpSummary(dump []byte) string {
	if len(dump) == 0 {
		return ""
	}
	d, err := minidump.Parse(dump)
	if err != nil || d.Exception == nil {
		return ""
	}
	x := d.Exception
	where, ok := d.Locate(x.Address)
	if !ok {
		where = fmt.Sprintf("0x%x (in no module)", x.Address)
	}
	what := exceptionNames[x.Code]
	if what == "" {
		what = "exception"
	}
	line := fmt.Sprintf("**Dump, read on arrival:** `0x%08x` %s at `%s`", x.Code, what, where)
	if x.Code == 0xc0000005 && len(x.Params) >= 2 {
		verb := map[uint64]string{0: "reading", 1: "writing", 8: "executing"}[x.Params[0]]
		if verb == "" {
			verb = fmt.Sprintf("access kind %d at", x.Params[0])
		}
		line += fmt.Sprintf(", %s `0x%x`", verb, x.Params[1])
	}
	line += fmt.Sprintf(", thread %d.", x.ThreadID)
	var stack []string
	for _, v := range d.StackModules(12, 1024) {
		s, _ := d.Locate(v)
		stack = append(stack, "`"+strings.ReplaceAll(s, "`", "'")+"`")
	}
	if len(stack) > 0 {
		line += "\nStack values that point into a module, from rsp up: " + strings.Join(stack, ", ") + "."
	}
	return line
}

// reportDump is the dump an issue summarises: Azimuth's crash.dmp, else the first minidump the report attached (the Mod Platform's).
func reportDump(r *Report) []byte {
	if r.HasDump {
		return r.Dump
	}
	for _, p := range r.Attachments {
		if b, ok := p.V.([]byte); ok && strings.HasSuffix(strings.ToLower(p.K), ".dmp") {
			return b
		}
	}
	return nil
}
