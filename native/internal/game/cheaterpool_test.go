package game

import (
	"os"
	"path/filepath"
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
