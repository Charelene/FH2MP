package game

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func lightingTestRequest(v2 bool, doors uint16) []byte {
	n, kind := lightingRequest, byte(1)
	if v2 {
		n, kind = lighting2Request, 3
	}
	d := make([]byte, n)
	copy(d, sideHeader[:9])
	binary.LittleEndian.PutUint16(d[9:], uint16(n))
	copy(d[11:19], lightingMagic)
	d[19], d[52], d[53] = kind, 4, 5
	if v2 {
		binary.LittleEndian.PutUint16(d[55:], doors)
	}
	return d
}

// v2 lighting requests carry the open parts; v1 requests stay as they were; a v2 reply record holds the parts, a v1 record does not.
func TestLightingV2(t *testing.T) {
	if v2, doors, _, ok := lightingParse(lightingTestRequest(true, 0x203)); !ok || !v2 || doors != 0x203 {
		t.Fatalf("v2 request: ok %v v2 %v parts %#x", ok, v2, doors)
	}
	if v2, doors, _, ok := lightingParse(lightingTestRequest(false, 0)); !ok || v2 || doors != 0 {
		t.Fatalf("v1 request: ok %v v2 %v parts %#x", ok, v2, doors)
	}
	if _, _, _, ok := lightingParse(lightingTestRequest(true, 1<<lightingDoorBits)); ok {
		t.Fatal("a v2 request with a part bit past the list was accepted")
	}
	bad := lightingTestRequest(true, 1)
	bad[19] = 1
	if _, _, _, ok := lightingParse(bad); ok {
		t.Fatal("a v2-length request with the v1 type was accepted")
	}
	st := &LightState{Car: 9, Flags: 3, Doors: 0x203}
	if got := lightingRecord(true, 2, st); !bytes.Equal(got, []byte{2, 9, 3, 0x03, 0x02, 0}) {
		t.Fatalf("v2 record % x", got)
	}
	if got := lightingRecord(false, 2, st); !bytes.Equal(got, []byte{2, 9, 3, 0}) {
		t.Fatalf("v1 record % x", got)
	}
}
