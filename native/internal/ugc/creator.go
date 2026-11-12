package ugc

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
)

// The creator stamp of a livery design (prod 2026-11-12, #255). A design's container is 'vlrc', version 1, three u32, a byte and a u32 (0x19 bytes,
// CCarLivery::Serialize game+0x122e620), then the layer block (u32 0x6c767279, 'yrvl' in the file) + u32 version whose first field is the creator's xuid, u64 LE (the layer reader
// game+0x122e8c0 reads it into +0x1f0) [C: every player-made design on prod carries its uploader's xuid there]. The FH4 conversions of 2026-10-29 and
// 2026-11-01 carry the xuid of the person who converted them: that player's game took them as its own designs, kept no versioned id when it applied
// one, and other players got the car's plain base livery.
const (
	liveryLayerBlockAt = 0x19
	liveryCreatorAt    = 0x21
	liveryHeadLen      = liveryCreatorAt + 8
)

// liveryHead inflates the first n bytes of a design in the game's compression-stream form (u32 LE compressed size, u32 LE raw size, zlib).
func liveryHead(blob []byte, n int) ([]byte, bool) {
	if len(blob) < 8 {
		return nil, false
	}
	comp, raw := int(binary.LittleEndian.Uint32(blob)), int(binary.LittleEndian.Uint32(blob[4:]))
	if len(blob) != 8+comp || raw < n || raw > MaxLiveryRaw {
		return nil, false
	}
	zr, err := zlib.NewReader(bytes.NewReader(blob[8:]))
	if err != nil {
		return nil, false
	}
	head := make([]byte, n)
	if _, err := io.ReadFull(zr, head); err != nil {
		return nil, false
	}
	return head, true
}

// validLiveryHead says whether the start of a container is a design with a creator stamp: 'vlrc' version 1 and a layer block ('yrvl') of version 1..5.
func validLiveryHead(c []byte) bool {
	if len(c) < liveryHeadLen || string(c[:4]) != "vlrc" || binary.LittleEndian.Uint32(c[4:]) != 1 || string(c[liveryLayerBlockAt:liveryLayerBlockAt+4]) != "yrvl" {
		return false
	}
	v := binary.LittleEndian.Uint32(c[liveryLayerBlockAt+4:])
	return v >= 1 && v <= 5
}

// LiveryCreator is the creator xuid stamped in a design; ok is false when the blob is not a design this layout covers. Only the first 0x29 bytes are
// inflated.
func LiveryCreator(blob []byte) (xuid int64, ok bool) {
	head, ok := liveryHead(blob, liveryHeadLen)
	if !ok || !validLiveryHead(head) {
		return 0, false
	}
	return int64(binary.LittleEndian.Uint64(head[liveryCreatorAt:])), true
}

// RestampLiveryCreator returns the design with xuid as its creator, in the same compression-stream form; every other byte of the container is kept.
func RestampLiveryCreator(blob []byte, xuid int64) ([]byte, error) {
	c, why := liveryChunk(blob)
	if c == nil {
		return nil, fmt.Errorf("not a livery chunk: %s", why)
	}
	if !validLiveryHead(c) {
		return nil, fmt.Errorf("not a vlrc design with a layer block")
	}
	c = append([]byte(nil), c...)
	binary.LittleEndian.PutUint64(c[liveryCreatorAt:], uint64(xuid))
	var z bytes.Buffer
	zw := zlib.NewWriter(&z)
	if _, err := zw.Write(c); err != nil {
		return nil, err
	}
	if err := zw.Close(); err != nil {
		return nil, err
	}
	out := make([]byte, 8, 8+z.Len())
	binary.LittleEndian.PutUint32(out, uint32(z.Len()))
	binary.LittleEndian.PutUint32(out[4:], uint32(len(c)))
	return append(out, z.Bytes()...), nil
}
