package pngtool

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"testing"
)

type px struct {
	W   int    `json:"w"`
	H   int    `json:"h"`
	Ch  int    `json:"ch"`
	Sha string `json:"sha"`
}

type fixture struct {
	Cases []struct {
		Png     string `json:"png"`
		Decoded px     `json:"decoded"`
		Down    []struct {
			Nw      int `json:"nw"`
			Nh      int `json:"nh"`
			Decoded px  `json:"decoded"`
		} `json:"down"`
	} `json:"cases"`
	Bad []struct {
		Name   string  `json:"name"`
		Png    string  `json:"png"`
		MaxDim *int    `json:"max_dim"`
		Err    *string `json:"err"`
	} `json:"bad"`
}

func pixels(t *testing.T, png []byte) px {
	im, err := Decode(png, MaxDim)
	if err != nil {
		t.Fatal(err)
	}
	h := sha256.New()
	for _, r := range im.Rows {
		h.Write(r)
	}
	return px{im.W, im.H, im.Ch, hex.EncodeToString(h.Sum(nil))}
}

func TestMatchesPython(t *testing.T) {
	raw, err := os.ReadFile("../../testdata/png.json")
	if err != nil {
		t.Skip(err)
	}
	var f fixture
	if err := json.Unmarshal(raw, &f); err != nil {
		t.Fatal(err)
	}
	for i, c := range f.Cases {
		png, _ := hex.DecodeString(c.Png)
		if got := pixels(t, png); got != c.Decoded {
			t.Fatalf("case %d decode: %+v want %+v", i, got, c.Decoded)
		}
		for _, d := range c.Down {
			small, err := Downscale(png, d.Nw, d.Nh)
			if err != nil {
				t.Fatal(err)
			}
			if got := pixels(t, small); got != d.Decoded {
				t.Fatalf("case %d downscale %dx%d: %+v want %+v", i, d.Nw, d.Nh, got, d.Decoded)
			}
		}
	}
	for _, b := range f.Bad {
		png, _ := hex.DecodeString(b.Png)
		max := MaxDim
		if b.MaxDim != nil {
			max = *b.MaxDim
		}
		_, err := Decode(png, max)
		switch {
		case b.Err == nil && err != nil:
			t.Errorf("%s: unexpected error %v", b.Name, err)
		case b.Err != nil && err == nil:
			t.Errorf("%s: accepted, Python says %q", b.Name, *b.Err)
		case b.Err != nil && err.Error() != *b.Err:
			t.Errorf("%s: error %q, Python %q", b.Name, err, *b.Err)
		}
	}
}
