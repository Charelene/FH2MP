package game

import (
	"strings"
	"testing"

	"fh2mp/internal/addons"
	"fh2mp/internal/catalog"
	"fh2mp/internal/parts"
	"fh2mp/internal/playersetup"
	"fh2mp/internal/pyrand"
	"github.com/Charelene/encore-paddock/bits"
)

// PoliceLightbars' hood (2038001, the 4C's amber bar) on a player whose game has the add-on: the setup goes on as the stock hood for everyone,
// with the setup as sent kept for the players whose games have the add-on too; nothing is repaired and the owner is not told anything.
// Without the add-on, or with another foreign part next to it, it is repaired like any part the server's database lacks.
func TestLightbarHoodReachesOnlyAddonPlayers(t *testing.T) {
	if PartPolicy != "fix" {
		t.Skip("part policy", PartPolicy)
	}
	r := newLoadRig(t, 3)
	w := r.w
	sender, with, without := r.peers[0], r.peers[1], r.peers[2]
	c := w.cat()
	stock, ok := parts.StockList(c.Parts, 2038)
	if !ok || len(parts.IntegrityErrors(c.Parts, stock)) > 0 {
		t.Skip("the catalog has no clean stock part list of ALF_4C_14")
	}
	hood := -1
	for slot, table := range c.Parts.SlotTable {
		if table == "CarBodyHood" {
			hood = slot
		}
	}
	if hood < 0 || stock[hood] != 2038000 {
		t.Fatalf("the 4C's stock hood is %v in slot %d, want 2038000", stock, hood)
	}
	if _, known := c.Parts.TableIDs["CarBodyHood"][2038001]; known {
		t.Fatal("the server's catalog has the lightbar hood: the test needs it unknown")
	}
	barred := append([]uint32(nil), stock...)
	barred[hood] = 2038001
	encode := func(words []uint32) (int, bits.Int) {
		t.Helper()
		pv, err := playersetup.PartsValue(words)
		if err != nil {
			t.Fatal(err)
		}
		size, payload, err := playersetup.Encode(map[int]bits.Int{playersetup.Parts: pv})
		if err != nil {
			t.Fatal(err)
		}
		return size, payload
	}
	sent := func(size int, payload bits.Int) []uint32 {
		t.Helper()
		f, err := playersetup.Fields(size, payload)
		if err != nil {
			t.Fatal(err)
		}
		return playersetup.PartWords(f[playersetup.Parts].Value)
	}
	hub := func(set map[uint64]map[string]bool) {
		w.Hub = &Hub{Addons: set, Clk: r.clk, syncer: catalog.NewSyncer("")}
	}

	hub(map[uint64]map[string]bool{sender.Xuid: {addons.Lightbars: true}, with.Xuid: {addons.Lightbars: true}})
	size, payload := encode(barred)
	repaired, hide, n, out := w.CheckParts(sender, size, payload, 1)
	if repaired || hide || !w.addonApplied {
		t.Fatalf("lightbar with the add-on: repaired %v hidden %v real version kept %v (want false false true)", repaired, hide, w.addonApplied)
	}
	if got := sent(n, out); got[hood] != 2038000 || len(parts.IntegrityErrors(c.Parts, got)) > 0 {
		t.Fatalf("the others' version has hood %d (want the stock 2038000) and %d integrity errors", got[hood], len(parts.IntegrityErrors(c.Parts, got)))
	}
	if sender.PartsRepairCar != nil || len(sender.RepairNotified) > 0 {
		t.Fatal("the owner's car was marked repaired, or the owner was told about it")
	}
	if !w.RealCarsFor(sender, with) || w.RealCarsFor(sender, without) {
		t.Fatalf("RealCarsFor: a player with the add-on %v, without %v (want true false)", w.RealCarsFor(sender, with), w.RealCarsFor(sender, without))
	}

	// the stock hood itself: nothing to keep
	size, payload = encode(stock)
	if _, _, _, _ = w.CheckParts(sender, size, payload, 1); w.addonApplied {
		t.Fatal("a stock car got a real version kept")
	}

	// a lightbar and a camshaft no database has: repaired for everyone, no real version
	worse := append([]uint32(nil), barred...)
	worse[12] = 987654321
	size, payload = encode(worse)
	if repaired, _, _, _ = w.CheckParts(sender, size, payload, 1); !repaired || w.addonApplied {
		t.Fatalf("lightbar plus a foreign camshaft: repaired %v, real version kept %v (want true false)", repaired, w.addonApplied)
	}

	// a player whose game does not have the add-on (its car database is the plain one): a foreign part like any other
	sender.PartsRepairCar = nil
	hub(map[uint64]map[string]bool{with.Xuid: {addons.Lightbars: true}})
	size, payload = encode(barred)
	if repaired, _, _, _ = w.CheckParts(sender, size, payload, 1); !repaired || w.addonApplied {
		t.Fatalf("lightbar without the add-on: repaired %v, real version kept %v (want true false)", repaired, w.addonApplied)
	}
}

// "addons <xuid> lightbars" from the LSP sets the player's add-ons, "-" forgets them, an unknown name is ignored.
func TestAddonsControl(t *testing.T) {
	clk := &fakeClock{t: 1000.0}
	cat, err := catalog.Load()
	if err != nil {
		t.Skip("catalog not available:", err)
	}
	var lines []string
	h, err := NewHub(HubConfig{Clock: clk, Rng: pyrand.New(1), Sock: &fakeSock{clk: clk}, Out: NewLogOut(func(s string) error { lines = append(lines, s); return nil }),
		Catalog: cat, CtlSend: func([]byte, Addr) {}, Sinks: &Sinks{}})
	if err != nil {
		t.Fatal(err)
	}
	h.ControlText("addons 90000e961ddae lightbars", nil)
	if !h.Addons[0x90000e961ddae][addons.Lightbars] {
		t.Fatalf("add-ons after the message: %v", h.Addons)
	}
	h.ControlText("addons 90000e961ddae nitro", nil)
	if len(h.Addons[0x90000e961ddae]) != 0 {
		t.Fatalf("a message naming only an unknown add-on left %v", h.Addons)
	}
	h.ControlText("addons 90000e961ddae lightbars", nil)
	h.ControlText("addons 90000e961ddae -", nil)
	if _, ok := h.Addons[0x90000e961ddae]; ok {
		t.Fatalf("\"-\" did not forget the add-ons: %v", h.Addons)
	}
	log := strings.Join(lines, "")
	if !strings.Contains(log, "add-ons of 90000e961ddae: lightbars") || !strings.Contains(log, "add-ons of 90000e961ddae forgotten") {
		t.Fatalf("control lines: %q", log)
	}
}
