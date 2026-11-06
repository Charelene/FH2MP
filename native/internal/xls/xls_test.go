package xls

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"testing"

	"fh2mp/internal/db"
)

type vec struct {
	Name    string `json:"name"`
	Default string `json:"default"`
	Filled  string `json:"filled"`
	Decoded any    `json:"decoded"`
	Error   string `json:"error"`
}

// jsonable mirrors vectors_xls.py: bytes as {"hex": ...}, structs as [[name, value], ...].
func jsonable(v any) any {
	switch x := v.(type) {
	case []byte:
		return map[string]any{"hex": hex.EncodeToString(x)}
	case [16]byte:
		return map[string]any{"hex": hex.EncodeToString(x[:])}
	case *Struct:
		out := make([]any, 0, len(x.Names))
		for _, k := range x.Names {
			out = append(out, []any{k, jsonable(x.Vals[k])})
		}
		return out
	case []any:
		out := make([]any, len(x))
		for i, e := range x {
			out[i] = jsonable(e)
		}
		return out
	case int64:
		return float64(x)
	case uint64:
		return float64(x)
	}
	return v
}

func TestCodecMatchesPython(t *testing.T) {
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
	checked := 0
	for _, v := range vecs {
		fields, err := sc.CallStruct(v.Name)
		if err != nil {
			t.Errorf("%s: %v", v.Name, err)
			continue
		}
		if v.Error != "" {
			continue
		}
		def, err := sc.DefaultStruct(fields)
		if err != nil {
			t.Errorf("%s default: %v", v.Name, err)
			continue
		}
		b, err := sc.Encode(fields, def)
		if err != nil || hex.EncodeToString(b) != v.Default {
			t.Errorf("%s: default encodes to %x (err %v), want %s", v.Name, b, err, v.Default)
			continue
		}
		data, _ := hex.DecodeString(v.Filled)
		dec, left, err := sc.Decode(fields, data)
		if err != nil || left != 0 {
			t.Errorf("%s: decode: left %d err %v", v.Name, left, err)
			continue
		}
		again, err := sc.Encode(fields, dec)
		if err != nil || hex.EncodeToString(again) != v.Filled {
			t.Errorf("%s: re-encode differs (err %v)", v.Name, err)
			continue
		}
		got, _ := json.Marshal(jsonable(dec))
		want, _ := json.Marshal(v.Decoded)
		if string(got) != string(want) {
			t.Errorf("%s: decoded value differs\n got %.300s\nwant %.300s", v.Name, got, want)
			continue
		}
		checked++
	}
	t.Log(fmt.Sprintf("%d call structs checked against the Python codec", checked))
}
