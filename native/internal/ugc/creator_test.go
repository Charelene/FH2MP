package ugc

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"testing"
)

// chunk wraps a container in the game's compression-stream form.
func chunk(t *testing.T, c []byte) []byte {
	t.Helper()
	var z bytes.Buffer
	zw := zlib.NewWriter(&z)
	zw.Write(c)
	zw.Close()
	out := make([]byte, 8)
	binary.LittleEndian.PutUint32(out, uint32(z.Len()))
	binary.LittleEndian.PutUint32(out[4:], uint32(len(c)))
	return append(out, z.Bytes()...)
}

// design is a container with the recovered head (the layout of design 21640) and a stand-in body.
func design(creator int64) []byte {
	c := make([]byte, 0x29, 0x80)
	copy(c, "vlrc")
	binary.LittleEndian.PutUint32(c[4:], 1)
	binary.LittleEndian.PutUint32(c[0x10:], 1033)
	copy(c[0x19:], "yrvl")
	binary.LittleEndian.PutUint32(c[0x1d:], 5)
	binary.LittleEndian.PutUint64(c[0x21:], uint64(creator))
	return append(c, bytes.Repeat([]byte{0x5a, 0x00, 0xff}, 30)...)
}

func TestLiveryCreatorRestamp(t *testing.T) {
	const converter, uploader = int64(0x90000f26391bc), int64(0x90000358947eb)
	orig := design(converter)
	blob := chunk(t, orig)
	if x, ok := LiveryCreator(blob); !ok || x != converter {
		t.Fatalf("creator %#x ok=%v, want %#x", x, ok, converter)
	}
	fixed, err := RestampLiveryCreator(blob, uploader)
	if err != nil {
		t.Fatal(err)
	}
	if x, ok := LiveryCreator(fixed); !ok || x != uploader {
		t.Fatalf("after the restamp: creator %#x ok=%v", x, ok)
	}
	c := LiveryChunk(fixed)
	want := append([]byte(nil), orig...)
	binary.LittleEndian.PutUint64(want[0x21:], uint64(uploader))
	if !bytes.Equal(c, want) {
		t.Fatal("the restamp changed more than the creator")
	}
	for name, bad := range map[string][]byte{
		"not vlrc":        chunk(t, append([]byte("xxxx"), orig[4:]...)),
		"no layer block":  chunk(t, append(append(append([]byte(nil), orig[:0x19]...), "nope"...), orig[0x1d:]...)),
		"short":           chunk(t, orig[:0x20]),
		"not a chunk":     []byte("vlrc but plain"),
		"truncated chunk": blob[:len(blob)-4],
	} {
		if _, ok := LiveryCreator(bad); ok {
			t.Errorf("%s: read a creator", name)
		}
		if _, err := RestampLiveryCreator(bad, uploader); err == nil {
			t.Errorf("%s: restamped", name)
		}
	}
}
