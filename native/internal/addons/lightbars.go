// Package addons knows the add-on mods that put parts on stock cars which only some players' games can draw, and what the servers do about
// them: which part ids are theirs, the stock part each stands for, and the database rows a game needs to load a car that has one.
//
// PoliceLightbars (suderek22/FH2_Lightbars, MIT, release v0.2.2, zip sha256 52f7a7a0...c6172e) adds an amber and a blue roof lightbar to 31
// stock cars as two extra Hood upgrades each. Its fh2-mods\PoliceLightbars.dll registers the rows with the FH2 Mod Platform's add_sql; the
// statements below are that DLL's own, byte for byte (strings at file offsets 0x17341, 0x173d6 and 0x1765a of the 0.2.2 DLL, 151,552 bytes)
// [confirmed: the DLL's strings]. Each car's zip in fh2-cars\PoliceLightbars names the two ids in its carbin's Hood part (the repo's
// AddRoofUpgrades.cs) [confirmed: the build script].
package addons

import "strings"

// Lightbars is the add-on's name in the LSP's control messages and the car database variant's path.
const Lightbars = "lightbars"

// LightbarsVersion is the PoliceLightbars release the rows and ids are taken from.
const LightbarsVersion = "0.2.2"

// lightbarFirst: a car body's stock hood row (Id = CarBodyID) -> the offset of its amber lightbar hood (the blue one is the next id). The
// 4C and the Charger take +1/+2 (Sequence 2/3), the other 29 cars +90/+91 (their bodies have other hood upgrades at +1 and up).
var lightbarFirst = map[uint32]uint32{
	2038000: 1, // ALF_4C_14
	1422000: 1, // DOD_ChargerSRT8_12
	1115000: 90, 2180000: 90, 1220000: 90, 1388000: 90, 1350000: 90, 1626000: 90, 1419000: 90, 2177000: 90, 1620000: 90, 1131000: 90,
	1460000: 90, 2155000: 90, 1345000: 90, 2108000: 90, 1522000: 90, 2110000: 90, 1221000: 90, 1658000: 90, 1627000: 90, 1525000: 90,
	1202000: 90, 1009000: 90, 2002000: 90, 445000: 90, 2293000: 90, 1060000: 90, 2393000: 90, 1231000: 90, 2156000: 90,
}

// LightbarBodies is how many car bodies have lightbar hoods.
func LightbarBodies() int { return len(lightbarFirst) }

// LightbarHood: word is one of PoliceLightbars' hood ids -> the stock hood row of the same body (what a game without the add-on draws).
func LightbarHood(word uint32) (stock uint32, ok bool) {
	body := word / 1000 * 1000
	first, ok := lightbarFirst[body]
	if !ok || (word-body != first && word-body != first+1) {
		return 0, false
	}
	return body, true
}

// LightbarHoods lists every lightbar hood id (amber, then blue, per body), for tests and logs.
func LightbarHoods() []uint32 {
	out := make([]uint32, 0, 2*len(lightbarFirst))
	for body, first := range lightbarFirst {
		out = append(out, body+first, body+first+1)
	}
	return out
}

// LightbarsStatements are what PoliceLightbars.dll 0.2.2 runs on the game's database: two part makers and the two hoods of each body. Run on the
// server's database, it makes the copy that players with the add-on play with.
var LightbarsStatements = []string{
	"INSERT OR IGNORE INTO List_PartManufacturer (Id,PartManufacturer,LogoPath) VALUES (7101,'_&1568995346','decal146'),(7102,'_&1568995347','decal146');",
	"INSERT OR IGNORE INTO List_UpgradeCarBodyHood (Id,CarBodyID,Level,IsStock,Sequence,ManufacturerId,PartsStringId,MassDiff,WeightDistDiff,DragScale,WindInstabilityScale,Price,RequiresGraphics)\n" +
		"SELECT stock.CarBodyID + variant.Offset, stock.CarBodyID, 1, 0, variant.Sequence, variant.ManufacturerId, stock.PartsStringId, stock.MassDiff, stock.WeightDistDiff, stock.DragScale, stock.WindInstabilityScale, 100, 1\n" +
		"FROM List_UpgradeCarBodyHood stock\n" +
		"CROSS JOIN (SELECT 1 AS Offset, 2 AS Sequence, 7101 AS ManufacturerId UNION ALL SELECT 2, 3, 7102) variant\n" +
		"WHERE stock.CarBodyID IN (2038000, 1422000) AND stock.Id=stock.CarBodyID AND stock.IsStock=1;",
	"INSERT OR IGNORE INTO List_UpgradeCarBodyHood (Id,CarBodyID,Level,IsStock,Sequence,ManufacturerId,PartsStringId,MassDiff,WeightDistDiff,DragScale,WindInstabilityScale,Price,RequiresGraphics)\n" +
		"SELECT stock.CarBodyID + variant.UpgradeDelta, stock.CarBodyID, 1, 0, variant.UpgradeDelta, variant.ManufacturerId, stock.PartsStringId, stock.MassDiff, stock.WeightDistDiff, stock.DragScale, stock.WindInstabilityScale, 100, 1\n" +
		"FROM List_UpgradeCarBodyHood stock\n" +
		"CROSS JOIN (SELECT 90 AS UpgradeDelta, 7101 AS ManufacturerId UNION ALL SELECT 91, 7102) variant\n" +
		"WHERE stock.CarBodyID IN (1115000, 2180000, 1220000, 1388000, 1350000, 1626000, 1419000, 2177000, 1620000, 1131000, 1460000, 2155000, 1345000, 2108000, 1522000, 2110000, 1221000, 1658000, 1627000, 1525000, 1202000, 1009000, 2002000, 445000, 2293000, 1060000, 2393000, 1231000, 2156000) AND stock.Id=stock.CarBodyID AND stock.IsStock=1;",
}

// LightbarsSQL is the statements as the DLL holds them in one string, a line apart (the DLL's string also starts and ends with a line break).
var LightbarsSQL = strings.Join(LightbarsStatements, "\n")

// Known: name is an add-on the servers handle.
func Known(name string) bool { return name == Lightbars }
