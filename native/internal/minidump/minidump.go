// Package minidump reads what crash triage needs from a Windows x64 minidump: the exception, the modules, the threads and the captured memory
// (the MINIDUMP_* structures of Microsoft's minidumpapiset.h). It reads files players upload, so every offset and size is checked: a bad one is
// an error or a missing part, never a panic.
package minidump

import (
	"encoding/binary"
	"errors"
	"fmt"
	"sort"
	"strings"
	"unicode/utf16"
)

// Stream types used here (MINIDUMP_STREAM_TYPE).
const (
	threadListStream   = 3
	moduleListStream   = 4
	memoryListStream   = 5
	exceptionStream    = 6
	memory64ListStream = 9
)

// Module is one loaded image; Name is the file name without its folder (a full path can name the player's user folder).
type Module struct {
	Name       string
	Base, Size uint64
}

// Thread is one thread with its CONTEXT record.
type Thread struct {
	ID      uint32
	Context []byte
}

// Range is one captured block of the process's memory.
type Range struct {
	Start uint64
	Data  []byte
}

// Exception is the exception the dump was written for.
type Exception struct {
	ThreadID uint32
	Code     uint32
	Address  uint64
	Params   []uint64
	Context  []byte
}

// Dump is the parsed file.
type Dump struct {
	Modules   []Module
	Threads   []Thread
	Ranges    []Range
	Exception *Exception
}

// ErrNotMinidump: the data does not start with the minidump signature.
var ErrNotMinidump = errors.New("not a minidump")

type reader []byte

func (b reader) u32(off uint64) (uint32, bool) {
	if off+4 > uint64(len(b)) {
		return 0, false
	}
	return binary.LittleEndian.Uint32(b[off:]), true
}

func (b reader) u64(off uint64) (uint64, bool) {
	if off+8 > uint64(len(b)) {
		return 0, false
	}
	return binary.LittleEndian.Uint64(b[off:]), true
}

func (b reader) slice(off, n uint64) ([]byte, bool) {
	if off > uint64(len(b)) || n > uint64(len(b))-off {
		return nil, false
	}
	return b[off : off+n], true
}

// str reads a MINIDUMP_STRING (a byte length, then UTF-16).
func (b reader) str(off uint64) (string, bool) {
	n, ok := b.u32(off)
	if !ok || n > 4096 {
		return "", false
	}
	raw, ok := b.slice(off+4, uint64(n))
	if !ok {
		return "", false
	}
	w := make([]uint16, len(raw)/2)
	for i := range w {
		w[i] = binary.LittleEndian.Uint16(raw[2*i:])
	}
	return string(utf16.Decode(w)), true
}

// Parse reads a minidump. A stream that does not fit the file is skipped; the parts that do fit are kept.
func Parse(data []byte) (*Dump, error) {
	b := reader(data)
	if len(b) < 32 || string(b[:4]) != "MDMP" {
		return nil, ErrNotMinidump
	}
	count, _ := b.u32(8)
	dir, _ := b.u32(12)
	if count > 1024 {
		return nil, fmt.Errorf("minidump: %d streams in the directory", count)
	}
	d := &Dump{}
	for i := uint64(0); i < uint64(count); i++ {
		e := uint64(dir) + 12*i
		typ, ok1 := b.u32(e)
		rva, ok2 := b.u32(e + 8)
		if !ok1 || !ok2 {
			return nil, fmt.Errorf("minidump: the stream directory ends inside the file's entry %d", i)
		}
		off := uint64(rva)
		switch typ {
		case threadListStream:
			n, _ := b.u32(off)
			for t := uint64(0); t < uint64(n) && t < 4096; t++ {
				o := off + 4 + 48*t
				id, ok := b.u32(o)
				stackStart, ok2 := b.u64(o + 24)
				stackSize, ok3 := b.u32(o + 32)
				stackRva, ok4 := b.u32(o + 36)
				ctxSize, ok5 := b.u32(o + 40)
				ctxRva, ok6 := b.u32(o + 44)
				if !(ok && ok2 && ok3 && ok4 && ok5 && ok6) {
					break
				}
				ctx, _ := b.slice(uint64(ctxRva), uint64(ctxSize))
				d.Threads = append(d.Threads, Thread{ID: id, Context: ctx})
				if mem, ok := b.slice(uint64(stackRva), uint64(stackSize)); ok && len(mem) > 0 {
					d.Ranges = append(d.Ranges, Range{Start: stackStart, Data: mem})
				}
			}
		case moduleListStream:
			n, _ := b.u32(off)
			for m := uint64(0); m < uint64(n) && m < 4096; m++ {
				o := off + 4 + 108*m
				base, ok := b.u64(o)
				size, ok2 := b.u32(o + 8)
				nameRva, ok3 := b.u32(o + 20)
				if !(ok && ok2 && ok3) {
					break
				}
				name, _ := b.str(uint64(nameRva))
				if k := strings.LastIndexAny(name, `\/`); k >= 0 {
					name = name[k+1:]
				}
				d.Modules = append(d.Modules, Module{Name: name, Base: base, Size: uint64(size)})
			}
		case memoryListStream:
			n, _ := b.u32(off)
			for m := uint64(0); m < uint64(n) && m < 1<<16; m++ {
				o := off + 4 + 16*m
				start, ok := b.u64(o)
				size, ok2 := b.u32(o + 8)
				at, ok3 := b.u32(o + 12)
				if !(ok && ok2 && ok3) {
					break
				}
				if mem, ok := b.slice(uint64(at), uint64(size)); ok {
					d.Ranges = append(d.Ranges, Range{Start: start, Data: mem})
				}
			}
		case memory64ListStream:
			n, ok := b.u64(off)
			at, ok2 := b.u64(off + 8)
			if !ok || !ok2 {
				continue
			}
			for m := uint64(0); m < n && m < 1<<20; m++ {
				o := off + 16 + 16*m
				start, ok := b.u64(o)
				size, ok2 := b.u64(o + 8)
				if !ok || !ok2 {
					break
				}
				mem, ok := b.slice(at, size)
				if !ok {
					break
				}
				d.Ranges = append(d.Ranges, Range{Start: start, Data: mem})
				at += size
			}
		case exceptionStream:
			tid, ok := b.u32(off)
			code, ok2 := b.u32(off + 8)
			addr, ok3 := b.u64(off + 24)
			np, ok4 := b.u32(off + 32)
			ctxSize, ok5 := b.u32(off + 160)
			ctxRva, ok6 := b.u32(off + 164)
			if !(ok && ok2 && ok3 && ok4 && ok5 && ok6) {
				continue
			}
			x := &Exception{ThreadID: tid, Code: code, Address: addr}
			for p := uint64(0); p < uint64(np) && p < 15; p++ {
				v, _ := b.u64(off + 40 + 8*p)
				x.Params = append(x.Params, v)
			}
			x.Context, _ = b.slice(uint64(ctxRva), uint64(ctxSize))
			d.Exception = x
		}
	}
	sort.SliceStable(d.Ranges, func(i, j int) bool { return d.Ranges[i].Start < d.Ranges[j].Start })
	d.addMappedGame()
	return d, nil
}

// GameBase is where Azimuth maps the Xbox game's image (its preferred base). Azimuth loads it itself, so the loader's module list, and with it
// the dump's, does not have it.
const GameBase = 0x140000000

// addMappedGame adds the game's image as module "game" when no module covers GameBase and the dump captured its PE header there (the
// image size comes from that header).
func (d *Dump) addMappedGame() {
	if _, ok := d.Locate(GameBase); ok {
		return
	}
	mz, ok := d.Qword(GameBase)
	if !ok || uint16(mz) != 0x5a4d {
		return
	}
	lfanew, ok := d.Qword(GameBase + 0x3c)
	if !ok || uint32(lfanew) > 0x1000 {
		return
	}
	nt := GameBase + uint64(uint32(lfanew))
	sig, ok := d.Qword(nt)
	if !ok || uint32(sig) != 0x4550 {
		return
	}
	size, ok := d.Qword(nt + 0x50) // IMAGE_NT_HEADERS64: Signature 4, FileHeader 20, then OptionalHeader.SizeOfImage at +0x38 of it
	if !ok || uint32(size) == 0 {
		return
	}
	d.Modules = append(d.Modules, Module{Name: "game", Base: GameBase, Size: uint64(uint32(size))})
}

// Locate names an address as "<module>+0x<offset>"; the game's image is "game" and Azimuth's "azimuth", as in FH2MP's logs.
func (d *Dump) Locate(addr uint64) (string, bool) {
	for _, m := range d.Modules {
		if addr >= m.Base && addr-m.Base < m.Size {
			return fmt.Sprintf("%s+0x%x", ShortName(m.Name), addr-m.Base), true
		}
	}
	return "", false
}

// ShortName is the name FH2MP's logs use for a module.
func ShortName(name string) string {
	switch strings.ToLower(name) {
	case "forza_durango_release_final.exe":
		return "game"
	case "azimuth.exe":
		return "azimuth"
	}
	return name
}

// Qword reads 8 bytes of captured memory.
func (d *Dump) Qword(addr uint64) (uint64, bool) {
	for _, r := range d.Ranges {
		if addr >= r.Start && addr-r.Start+8 <= uint64(len(r.Data)) {
			return binary.LittleEndian.Uint64(r.Data[addr-r.Start:]), true
		}
	}
	return 0, false
}

// Registers of an x64 CONTEXT record (offsets of winnt.h's CONTEXT for AMD64): Rax at 0x78, then Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8-R15, Rip.
var regNames = []string{"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"}

// Reg reads one register from a CONTEXT record.
func Reg(ctx []byte, name string) (uint64, bool) {
	for i, n := range regNames {
		if n == name {
			o := 0x78 + 8*i
			if o+8 > len(ctx) {
				return 0, false
			}
			return binary.LittleEndian.Uint64(ctx[o:]), true
		}
	}
	return 0, false
}

// StackModules returns, from the exception thread's rsp upward, the first n stack values that point into a module: the return addresses (and
// a few function pointers) a triage starts from. The words scanned are at most limit.
func (d *Dump) StackModules(n, limit int) []uint64 {
	if d.Exception == nil {
		return nil
	}
	rsp, ok := Reg(d.Exception.Context, "rsp")
	if !ok {
		return nil
	}
	var out []uint64
	for i := 0; i < limit && len(out) < n; i++ {
		v, ok := d.Qword(rsp + uint64(8*i))
		if !ok {
			break
		}
		if _, in := d.Locate(v); in {
			out = append(out, v)
		}
	}
	return out
}
