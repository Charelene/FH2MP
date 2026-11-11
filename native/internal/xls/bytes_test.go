package xls

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"os"
	"testing"

	"fh2mp/internal/db"
)

// prod 2026-11-06: the Go LSP read the scoreId of every UploadFile as empty, so no ghost matched its run (0 ghosts stored after the switch).
func TestBytesReadsAGuidField(t *testing.T) {
	id := [16]byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
	s := NewStruct()
	s.Set("scoreId", id)
	s.Set("data", []byte{2, 3})
	if got := s.Bytes("scoreId"); !bytes.Equal(got, id[:]) {
		t.Fatalf("GUID field read as %x, want %x", got, id)
	}
	if got := s.Bytes("data"); !bytes.Equal(got, []byte{2, 3}) {
		t.Fatalf("blob field read as %x", got)
	}
	if got := s.Bytes("absent"); got != nil {
		t.Fatalf("absent field read as %x", got)
	}
}

// The two calls that carry a score GUID, decoded through the game's own schema from the frozen vectors: the handlers' Bytes reads get the 16 bytes.
func TestScoreGuidCallsDecodeToBytes(t *testing.T) {
	dsn := os.Getenv("FH2MP_TEST_CATALOG_DSN")
	if dsn == "" {
		dsn = "postgresql://postgres@127.0.0.1:55432/fh2mp_cat"
	}
	d, err := db.Open(dsn)
	if err != nil {
		t.Skip("no catalog database:", err)
	}
	defer d.Close()
	sc, err := LoadSchema(d)
	if err != nil {
		t.Skip("no game schema:", err)
	}
	raw, err := os.ReadFile("../../testdata/xls.json")
	if err != nil {
		t.Skip(err)
	}
	var vecs []vec
	if err := json.Unmarshal(raw, &vecs); err != nil {
		t.Fatal(err)
	}
	want := map[string]struct{ field, hex string }{
		"ForzaWebServicesScoreboardUploadFile_Parameters":  {"scoreId", "0102030405060708090a0b0c0d0e0f10"},
		"ForzaWebServicesScoreboardNotifyRival_Parameters": {"myScoreId", "030405060708090a0b0c0d0e0f101112"},
	}
	seen := 0
	for _, v := range vecs {
		w, ok := want[v.Name]
		if !ok {
			continue
		}
		fields, err := sc.CallStruct(v.Name)
		if err != nil {
			t.Fatalf("%s: %v", v.Name, err)
		}
		data, _ := hex.DecodeString(v.Filled)
		dec, _, err := sc.Decode(fields, data)
		if err != nil {
			t.Fatalf("%s: decode: %v", v.Name, err)
		}
		if got := hex.EncodeToString(dec.Bytes(w.field)); got != w.hex {
			t.Errorf("%s.%s read as %q, want %s", v.Name, w.field, got, w.hex)
		}
		seen++
	}
	if seen != len(want) {
		t.Fatalf("found %d of %d vectors", seen, len(want))
	}
}
