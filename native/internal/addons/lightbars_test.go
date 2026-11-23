package addons

import (
	"crypto/sha256"
	"encoding/hex"
	"testing"
)

// The DLL's own string (PoliceLightbars.dll 0.2.2, file offsets 0x17340-0x179d0, 1680 bytes) hashes to this; a statement copied wrong fails.
func TestSQLIsTheDLLsOwn(t *testing.T) {
	sum := sha256.Sum256([]byte("\n" + LightbarsSQL + "\n"))
	if got := hex.EncodeToString(sum[:]); got != "974961d10715e826c3f88d1a542ff4d283455c6a6dcc9dd51b84beeb99c45527" {
		t.Fatalf("LightbarsSQL is not PoliceLightbars 0.2.2's: sha256 %s, %d bytes", got, len(LightbarsSQL)+2)
	}
}

func TestLightbarHood(t *testing.T) {
	cases := []struct {
		word, stock uint32
		ok          bool
	}{
		{2038001, 2038000, true}, {2038002, 2038000, true}, {2038003, 0, false}, {2038090, 0, false}, // the 4C: +1/+2 only
		{1422002, 1422000, true},
		{1115090, 1115000, true}, {1115091, 1115000, true}, {1115001, 0, false}, {1115092, 0, false}, // the 500: +90/+91 only
		{445090, 445000, true}, // a body under a million (NIS_SkylineGTRVSpecII_02's, ExtraPlacements.json)
		{2038000, 0, false},    // the stock hood itself
		{1216090, 0, false},    // a body without lightbars
		{0, 0, false}, {0xFFFFFFFF, 0, false},
	}
	for _, c := range cases {
		stock, ok := LightbarHood(c.word)
		if stock != c.stock || ok != c.ok {
			t.Errorf("LightbarHood(%d) = %d, %v; want %d, %v", c.word, stock, ok, c.stock, c.ok)
		}
	}
	if n := LightbarBodies(); n != 31 {
		t.Errorf("%d bodies, the release has 31 cars", n)
	}
	if n := len(LightbarHoods()); n != 62 {
		t.Errorf("%d hood ids, want 62", n)
	}
}
