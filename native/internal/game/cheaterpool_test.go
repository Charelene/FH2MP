package game

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"fh2mp/internal/omap"
)

// A tool report no longer flags anybody (NitroSystem stopped being a restricted tool 2026-11-07): loadFlags drops those entries and keeps the rest.
func TestLoadFlagsReleasesToolReports(t *testing.T) {
	path := filepath.Join(t.TempDir(), "cheaters.json")
	data := `{"1": [9e18, "restricted tools: nitrosystem"], "2": [9e18, "car policy: 3 unknown part(s)"], "3": [9e18, "restricted tools: nitrosystem,redline"]}`
	if err := os.WriteFile(path, []byte(data), 0o644); err != nil {
		t.Fatal(err)
	}
	h := &Hub{FlagFile: path, Flagged: omap.New[uint64, flagEnt]()}
	h.loadFlags()
	if h.FlagsError != "" {
		t.Fatalf("FlagsError %q", h.FlagsError)
	}
	if h.FlagsReleased != 2 {
		t.Errorf("released %d, want 2", h.FlagsReleased)
	}
	if keys := h.Flagged.Keys(); len(keys) != 1 || keys[0] != 2 {
		t.Errorf("flagged %v, want [2]", keys)
	}
}

// The panel's list carries hex xuid, reason and expiry, and drops expired entries.
func TestFlagStatusListsActiveFlags(t *testing.T) {
	h := &Hub{Flagged: omap.New[uint64, flagEnt](), Clk: &fakeClock{}}
	h.Flagged.Set(0x90000b46ba5b3, flagEnt{until: 1e18, reason: "speed 575 km/h"})
	h.Flagged.Set(0x2, flagEnt{until: -1, reason: "gone"})
	out := jsonDumps(h.flagStatus())
	if !strings.Contains(out, `"xuid": "90000b46ba5b3"`) || !strings.Contains(out, "speed 575 km/h") || strings.Contains(out, "gone") {
		t.Errorf("flag status %s", out)
	}
}
