package parts

import (
	"database/sql"
	"fmt"
	"path/filepath"
	"reflect"
	"testing"

	"fh2mp/internal/catalog"
)

const packCar = 9000

// A car pack's car (the FH2 Mod Platform's make_car_pack.py) gets rows of its own (car id x 1000) for the tables keyed by Ordinal, but its body
// row installs the base car's body (CarBodyID 368000), so its bumpers, hood, side skirts and chassis rows are the base car's. They used to be
// taken to belong to the pack car (body row / 1000), and every such car was "repaired" to a list without them (PR #426, ported from Python).
func TestPackCarOnItsBaseCarsBodyIsANormalCar(t *testing.T) {
	cat, err := catalog.Load()
	if err != nil {
		t.Skip("catalog not available:", err)
	}
	tables := cat.Clone().Parts
	path := filepath.Join(t.TempDir(), "gamedbRC.slt")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	exec := func(q string, args ...any) {
		if _, err := db.Exec(q, args...); err != nil {
			t.Fatalf("%s: %v", q, err)
		}
	}
	for _, x := range []struct{ name, col string }{{"Engine", "EngineID"}, {"Drivetrain", "DrivetrainID"}, {"Motor", "MotorID"}, {"CarBody", "CarBodyID"}} {
		exec(fmt.Sprintf("CREATE TABLE List_Upgrade%s (Id INTEGER, Ordinal INTEGER, %s INTEGER, IsStock INTEGER)", x.name, x.col))
	}
	var own []string
	for name := range tables.TableIDs {
		if tables.OwnerKind[name] == "car" && !BodyTables[name] && name != "Engine" && name != "Drivetrain" && name != "Motor" && name != "CarBody" {
			own = append(own, name)
			exec(fmt.Sprintf("CREATE TABLE List_Upgrade%s (Id INTEGER, Ordinal INTEGER, IsStock INTEGER)", name))
			exec(fmt.Sprintf("INSERT INTO List_Upgrade%s VALUES (?, ?, 1)", name), packCar*1000, packCar)
		}
	}
	exec("INSERT INTO List_UpgradeEngine VALUES (?, ?, ?, 1)", packCar*1000, packCar, tables.Installs["Engine"][368000])
	exec("INSERT INTO List_UpgradeDrivetrain VALUES (?, ?, ?, 1)", packCar*1000, packCar, tables.Installs["Drivetrain"][368000])
	exec("INSERT INTO List_UpgradeCarBody VALUES (?, ?, 368000, 1)", packCar*1000, packCar)
	db.Close()

	ro, err := catalog.OpenRO(path)
	if err != nil {
		t.Fatal(err)
	}
	pd, err := catalog.ReadParts(ro)
	ro.Close()
	if err != nil {
		t.Fatal(err)
	}
	if pd.Bodies[packCar*1000] != 368 {
		t.Fatalf("the pack car's body row installs car %d's body, want 368", pd.Bodies[packCar*1000])
	}
	tables.Extend(pd)

	base, ok := StockList(tables, 368)
	if !ok {
		t.Skip("car 368 has no stock part list in this catalog")
	}
	words := append([]uint32(nil), base...)
	isOwn := map[string]bool{"Engine": true, "Drivetrain": true, "CarBody": true}
	for _, n := range own {
		isOwn[n] = true
	}
	for k, name := range tables.SlotTable {
		if isOwn[name] && words[k] != None {
			words[k] = packCar * 1000
		}
	}
	if words[34] != 368000 {
		t.Fatalf("slot 34 (front bumper) holds %d, want the base car's row 368000", words[34])
	}
	if errs := IntegrityErrors(tables, words); len(errs) != 0 {
		t.Fatalf("a stock pack car fails the part check: %+v", errs)
	}
	if got, ok := StockList(tables, packCar); !ok || !reflect.DeepEqual(got, words) {
		t.Fatalf("the pack car's stock list %v, want %v (the base car's body parts)", got, words)
	}
	if errs := IntegrityErrors(tables, base); len(errs) != 0 {
		t.Fatalf("the base car itself fails the part check now: %+v", errs)
	}
}

// A stock car's own body row needs no entry: nothing changes for the cars of the game.
func TestStockCarNeedsNoBodyEntry(t *testing.T) {
	tables := &catalog.Tables{TableIDs: map[string]map[uint32]struct{}{}, OwnerKind: map[string]string{}, RowOwner: map[string]map[uint32]int64{},
		Stock: map[string]map[int64]uint32{}, Installs: map[string]map[uint32]int64{}}
	added := tables.Extend(&catalog.PartData{Bodies: map[uint32]int64{368000: 368, packCar * 1000: 368}})
	if _, has := tables.BodyOf[368000]; added != 0 || has || tables.BodyOf[packCar*1000] != 368 {
		t.Fatalf("added %d, body entries %v: want 0 and only the pack car's row", added, tables.BodyOf)
	}
}
