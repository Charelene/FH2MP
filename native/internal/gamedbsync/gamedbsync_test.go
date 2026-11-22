package gamedbsync

import (
	"bytes"
	"database/sql"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/ulikunitz/xz"

	"fh2mp/internal/addons"
)

// a database with the two tables the lightbar statements read, laid out as the game's (only the columns they name), and a stock hood of
// the 4C, of the Abarth 500 (one of the +90 bodies) with another hood upgrade at +1, and of a body without lightbars
func tinyDb(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), FileName)
	db, err := sql.Open("sqlite", "file:"+strings.ReplaceAll(path, "\\", "/"))
	if err != nil {
		t.Fatal(err)
	}
	for _, s := range []string{
		"CREATE TABLE List_PartManufacturer (Id INTEGER PRIMARY KEY, PartManufacturer TEXT, LogoPath TEXT)",
		"CREATE TABLE List_UpgradeCarBodyHood (Id INTEGER PRIMARY KEY, CarBodyID INTEGER, Level INTEGER, IsStock INTEGER, Sequence INTEGER, ManufacturerId INTEGER, PartsStringId INTEGER, MassDiff REAL, WeightDistDiff REAL, DragScale REAL, WindInstabilityScale REAL, Price INTEGER, RequiresGraphics INTEGER)",
		"INSERT INTO List_UpgradeCarBodyHood VALUES (2038000, 2038000, 0, 1, 1, 0, 77, 0.5, 0.01, 1.0, 1.0, 0, 0)",
		"INSERT INTO List_UpgradeCarBodyHood VALUES (1115000, 1115000, 0, 1, 1, 0, 78, 0.0, 0.0, 1.0, 1.0, 0, 0)",
		"INSERT INTO List_UpgradeCarBodyHood VALUES (1115001, 1115000, 1, 0, 2, 3, 79, -2.0, 0.0, 0.98, 1.0, 900, 1)",
		"INSERT INTO List_UpgradeCarBodyHood VALUES (1216000, 1216000, 0, 1, 1, 0, 80, 0.0, 0.0, 1.0, 1.0, 0, 0)",
	} {
		if _, err := db.Exec(s); err != nil {
			t.Fatal(err)
		}
	}
	db.Close()
	return path
}

func unxz(t *testing.T, blob []byte) []byte {
	t.Helper()
	r, err := xz.NewReader(bytes.NewReader(blob))
	if err != nil {
		t.Fatal(err)
	}
	out, err := io.ReadAll(r)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func TestLightbarsCopyHasTheDLLsRows(t *testing.T) {
	path := tinyDb(t)
	before, _ := os.ReadFile(path)
	g := New(path)
	plain := g.Manifest()
	m := g.VariantManifest(addons.Lightbars)
	if plain == nil || m == nil {
		t.Fatalf("manifests: plain %v, lightbars %v", plain, m)
	}
	if m.Sha256 == plain.Sha256 || m.Path != "/v1/gamedb/lightbars/gamedbRC.slt.xz" {
		t.Fatalf("lightbars copy %+v next to %+v", m, plain)
	}
	if after, _ := os.ReadFile(path); !bytes.Equal(before, after) {
		t.Fatal("building the copy changed the served file")
	}
	if g.VariantOf(m.Sha256) != addons.Lightbars || g.VariantOf(plain.Sha256) != "" {
		t.Fatal("VariantOf does not tell the copy from the file")
	}
	copyPath := filepath.Join(t.TempDir(), "copy.slt")
	os.WriteFile(copyPath, unxz(t, g.VariantDownload(addons.Lightbars)), 0o644)
	db, err := sql.Open("sqlite", "file:"+strings.ReplaceAll(copyPath, "\\", "/")+"?mode=ro")
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	type row struct {
		id, body, level, stock, seq, maker, str, price, gfx int64
		mass                                                float64
	}
	got := map[int64]row{}
	rs, err := db.Query("SELECT Id, CarBodyID, Level, IsStock, Sequence, ManufacturerId, PartsStringId, Price, RequiresGraphics, MassDiff FROM List_UpgradeCarBodyHood")
	if err != nil {
		t.Fatal(err)
	}
	for rs.Next() {
		var r row
		rs.Scan(&r.id, &r.body, &r.level, &r.stock, &r.seq, &r.maker, &r.str, &r.price, &r.gfx, &r.mass)
		got[r.id] = r
	}
	rs.Close()
	want := map[int64]row{
		2038001: {2038001, 2038000, 1, 0, 2, 7101, 77, 100, 1, 0.5}, // the stock hood's string and mass, amber maker
		2038002: {2038002, 2038000, 1, 0, 3, 7102, 77, 100, 1, 0.5},
		1115090: {1115090, 1115000, 1, 0, 90, 7101, 78, 100, 1, 0},
		1115091: {1115091, 1115000, 1, 0, 91, 7102, 78, 100, 1, 0},
	}
	for id, w := range want {
		if got[id] != w {
			t.Errorf("hood %d = %+v, want %+v", id, got[id], w)
		}
	}
	if len(got) != 4+4 {
		t.Errorf("%d hood rows, want the 4 there were and 4 new: %v", len(got), got)
	}
	var makers int
	db.QueryRow("SELECT COUNT(*) FROM List_PartManufacturer WHERE Id IN (7101, 7102)").Scan(&makers)
	if makers != 2 {
		t.Errorf("%d of the two part makers", makers)
	}
}

// The real server database, where the rig has one (skipped elsewhere): every one of the 31 bodies gets its two hoods.
func TestLightbarsCopyOfTheServersDatabase(t *testing.T) {
	path := filepath.Join("..", "..", "..", "azimuth", "win1", "fh2mp", FileName)
	data, err := os.ReadFile(path)
	if err != nil {
		t.Skip("no server database on this machine:", err)
	}
	_, rows, err := buildVariant(data, addons.LightbarsStatements)
	if err != nil {
		t.Fatal(err)
	}
	if rows != 2+62 {
		t.Fatalf("%d rows added, want 2 part makers and 62 hoods", rows)
	}
}
