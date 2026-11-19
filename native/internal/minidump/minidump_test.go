package minidump

import (
	"encoding/binary"
	"testing"
	"unicode/utf16"
)

// builder writes a minidump with the MINIDUMP_* layouts of minidumpapiset.h; the expected values below are the ones put into it.
type builder struct {
	b       []byte
	streams [][3]uint32 // type, size, rva
}

func (w *builder) at() uint32          { return uint32(len(w.b)) }
func (w *builder) u32(v uint32)        { w.b = binary.LittleEndian.AppendUint32(w.b, v) }
func (w *builder) u64(v uint64)        { w.b = binary.LittleEndian.AppendUint64(w.b, v) }
func (w *builder) pad(n int)           { w.b = append(w.b, make([]byte, n)...) }
func (w *builder) put32(off, v uint32) { binary.LittleEndian.PutUint32(w.b[off:], v) }

func (w *builder) str(s string) uint32 {
	at := w.at()
	u := utf16.Encode([]rune(s))
	w.u32(uint32(2 * len(u)))
	for _, c := range u {
		w.b = binary.LittleEndian.AppendUint16(w.b, c)
	}
	return at
}

func (w *builder) blob(data []byte) uint32 { at := w.at(); w.b = append(w.b, data...); return at }

// context is a 0x4d0-byte CONTEXT with rsp and rip set.
func context(rsp, rip uint64) []byte {
	c := make([]byte, 0x4d0)
	binary.LittleEndian.PutUint64(c[0x98:], rsp)
	binary.LittleEndian.PutUint64(c[0xf8:], rip)
	return c
}

const (
	gameBase = 0x140000000
	gameSize = 0x3a18000
	azBase   = 0x7ff600000000
	azSize   = 0x400000
	stackAt  = 0x5f000
)

func sampleDump(t *testing.T) []byte {
	t.Helper()
	w := &builder{}
	w.b = append(w.b, "MDMP"...)
	w.u32(0xa793) // version
	w.u32(5)      // streams
	w.u32(32)     // directory rva
	w.pad(16)
	w.pad(5 * 12) // the directory, filled in below

	gameName := w.str(`C:\Users\someone\Games\ForzaHorizon2\Mount\forza_durango_release_final.exe`)
	azName := w.str(`C:\Users\someone\Games\Azimuth\azimuth.exe`)
	modules := w.at()
	w.u32(2)
	for _, m := range []struct {
		base uint64
		size uint32
		name uint32
	}{{gameBase, gameSize, gameName}, {azBase, azSize, azName}} {
		start := w.at()
		w.u64(m.base)
		w.u32(m.size)
		w.u32(0)
		w.u32(0)
		w.u32(m.name)
		w.pad(108 - int(w.at()-start))
	}
	w.streams = append(w.streams, [3]uint32{moduleListStream, w.at() - modules, modules})

	// the faulting thread's stack: rsp at stackAt+0x10; a return address into the game, a heap pointer, one into Azimuth
	stack := make([]byte, 0x40)
	binary.LittleEndian.PutUint64(stack[0x10:], 0x1234)            // not in a module
	binary.LittleEndian.PutUint64(stack[0x18:], gameBase+0x66a4a0) // game+0x66a4a0
	binary.LittleEndian.PutUint64(stack[0x20:], 0x2a0001000)       // heap
	binary.LittleEndian.PutUint64(stack[0x28:], azBase+0x1c60bc)   // azimuth+0x1c60bc
	stackRva := w.blob(stack)
	ctx := context(stackAt+0x10, gameBase+0x66a492)
	ctxRva := w.blob(ctx)

	threads := w.at()
	w.u32(1)
	w.u32(4242) // id
	w.pad(12)   // suspend count, priority class, priority
	w.u64(0)    // teb
	w.u64(stackAt)
	w.u32(uint32(len(stack)))
	w.u32(stackRva)
	w.u32(uint32(len(ctx)))
	w.u32(ctxRva)
	w.streams = append(w.streams, [3]uint32{threadListStream, w.at() - threads, threads})

	heap := make([]byte, 16)
	binary.LittleEndian.PutUint64(heap[8:], 0xdeadbeefcafe)
	heapRva := w.blob(heap)
	memory := w.at()
	w.u32(1)
	w.u64(0x2a0001000)
	w.u32(16)
	w.u32(heapRva)
	w.streams = append(w.streams, [3]uint32{memoryListStream, w.at() - memory, memory})

	exc := w.at()
	w.u32(4242)
	w.u32(0)
	w.u32(0xc0000094) // integer divide by zero
	w.u32(0)
	w.u64(0)
	w.u64(gameBase + 0x66a492)
	w.u32(2)
	w.u32(0)
	w.u64(0)
	w.u64(0x68)
	w.pad(13 * 8)
	w.u32(uint32(len(ctx)))
	w.u32(ctxRva)
	w.streams = append(w.streams, [3]uint32{exceptionStream, w.at() - exc, exc})

	// a stream type this reader does not use, pointing past the end: skipped
	w.streams = append(w.streams, [3]uint32{0x10, 48, 0xfffffff0})

	for i, s := range w.streams {
		o := uint32(32 + 12*i)
		w.put32(o, s[0])
		w.put32(o+4, s[1])
		w.put32(o+8, s[2])
	}
	return w.b
}

func TestParseReadsTheExceptionModulesAndStack(t *testing.T) {
	d, err := Parse(sampleDump(t))
	if err != nil {
		t.Fatal(err)
	}
	if len(d.Modules) != 2 || d.Modules[0].Name != "forza_durango_release_final.exe" || d.Modules[1].Name != "azimuth.exe" {
		t.Fatalf("modules %+v (names lose their folders)", d.Modules)
	}
	x := d.Exception
	if x == nil || x.ThreadID != 4242 || x.Code != 0xc0000094 || x.Address != gameBase+0x66a492 || len(x.Params) != 2 || x.Params[1] != 0x68 {
		t.Fatalf("exception %+v", x)
	}
	if at, ok := d.Locate(x.Address); !ok || at != "game+0x66a492" {
		t.Fatalf("located %q", at)
	}
	if rip, _ := Reg(x.Context, "rip"); rip != gameBase+0x66a492 {
		t.Fatalf("rip %#x", rip)
	}
	if v, ok := d.Qword(0x2a0001008); !ok || v != 0xdeadbeefcafe {
		t.Fatalf("heap qword %#x %v", v, ok)
	}
	got := d.StackModules(8, 64)
	if len(got) != 2 || got[0] != gameBase+0x66a4a0 || got[1] != azBase+0x1c60bc {
		t.Fatalf("stack %#x", got)
	}
	if at, _ := d.Locate(got[1]); at != "azimuth+0x1c60bc" {
		t.Fatalf("azimuth located %q", at)
	}
	if len(d.Threads) != 1 || d.Threads[0].ID != 4242 {
		t.Fatalf("threads %+v", d.Threads)
	}
}

func TestParseRefusesWhatIsNotAMinidump(t *testing.T) {
	if _, err := Parse([]byte("PK\x03\x04 not a dump at all, long enough")); err != ErrNotMinidump {
		t.Fatalf("got %v", err)
	}
	if _, err := Parse(nil); err != ErrNotMinidump {
		t.Fatalf("got %v", err)
	}
}

// Every prefix of a valid dump, and the dump with any one byte of its first 1 KB flipped, parses or fails without a panic.
func TestParseNeverPanicsOnACutOrDamagedFile(t *testing.T) {
	full := sampleDump(t)
	for n := 0; n <= len(full); n += 7 {
		d, err := Parse(full[:n])
		if err == nil {
			d.StackModules(8, 64)
		}
	}
	for i := 0; i < 1024 && i < len(full); i++ {
		b := append([]byte(nil), full...)
		b[i] ^= 0xff
		if d, err := Parse(b); err == nil {
			d.StackModules(8, 64)
			if d.Exception != nil {
				d.Locate(d.Exception.Address)
			}
		}
	}
}

// Azimuth maps the game's image itself, so a real dump's module list lacks it; its PE header in the captured memory names it "game".
func TestTheGameImageIsFoundFromItsHeader(t *testing.T) {
	w := &builder{}
	w.b = append(w.b, "MDMP"...)
	w.u32(0xa793)
	w.u32(2)
	w.u32(32)
	w.pad(16)
	w.pad(2 * 12)
	page := make([]byte, 0x200)
	page[0], page[1] = 'M', 'Z'
	binary.LittleEndian.PutUint32(page[0x3c:], 0x80)
	copy(page[0x80:], "PE\x00\x00")
	binary.LittleEndian.PutUint32(page[0x80+0x50:], gameSize)
	pageRva := w.blob(page)
	memory := w.at()
	w.u32(1)
	w.u64(gameBase)
	w.u32(uint32(len(page)))
	w.u32(pageRva)
	w.streams = append(w.streams, [3]uint32{memoryListStream, w.at() - memory, memory})
	exc := w.at()
	w.u32(1)
	w.u32(0)
	w.u32(0xc0000005)
	w.u32(0)
	w.u64(0)
	w.u64(gameBase + 0x12e0de0)
	w.u32(0)
	w.pad(4 + 15*8)
	w.u32(0)
	w.u32(0)
	w.streams = append(w.streams, [3]uint32{exceptionStream, w.at() - exc, exc})
	for i, s := range w.streams {
		o := uint32(32 + 12*i)
		w.put32(o, s[0])
		w.put32(o+4, s[1])
		w.put32(o+8, s[2])
	}
	d, err := Parse(w.b)
	if err != nil {
		t.Fatal(err)
	}
	if at, ok := d.Locate(d.Exception.Address); !ok || at != "game+0x12e0de0" {
		t.Fatalf("located %q %v", at, ok)
	}
	if _, ok := d.Locate(gameBase + gameSize); ok {
		t.Fatal("the image ends at SizeOfImage")
	}
}
