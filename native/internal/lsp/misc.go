package lsp

import (
	"fmt"
	"os"
	"time"

	"fh2mp/internal/drivatar"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/xls"
	"fh2mp/internal/xpaudit"
)

func init() {
	const drv = "Forza.WebServices.Drivatar"
	register(drv, "SaveDrivatarInformation", drvSave)
	register(drv, "GetDrivatarsForCache", drvForCache)
	register(drv, "GetDrivatarInformationForRace", drvForRace)
	register("Forza.WebServices.User", "GetPermissionsTable", permissionsTable)
	register("Forza.WebServices.User", "GetEconomySettings", economySettings)
	register("Forza.WebServices.Gifting", "RetrieveCreditsGift", giftCredits)
	register("Forza.WebServices.Gifting", "RetrieveCarGift", giftCar)
}

// ---------------------------------------------------------------- drivatars

func recInt(rec drivatar.Record, k string) int64 {
	v, _ := rec.Get(k)
	switch x := v.(type) {
	case int64:
		return x
	case uint64:
		return int64(x)
	case float64:
		return int64(x)
	}
	return 0
}

func drvSave(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, tag := r.Xuid, c.Tag(r.Xuid)
	segs, curves := req.List("segmentInformation"), req.List("curveTypes")
	_, refused, err := c.Drv.SaveUpload(xuid, tag, structToJSON(req.Get("info")), structToJSON(req.Get("curveTypes")), structToJSON(req.Get("segmentInformation")))
	if err != nil {
		return nil, err
	}
	if refused {
		c.Log(r, fmt.Sprintf("  refused a Drivatar upload from %#x: a value is not finite or out of range", xuid))
		return nil, nil
	}
	c.Log(r, fmt.Sprintf("  stored Drivatar upload from %#x (%d segments, %d curves)", xuid, len(segs), len(curves)))
	return nil, nil
}

func (c *Core) recStructs(recs []drivatar.Record) []any {
	out := make([]any, len(recs))
	for i, r := range recs {
		out[i] = jsonToStruct(pyjson.Obj(r))
	}
	return out
}

func drvForCache(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, tag := r.Xuid, c.Tag(r.Xuid)
	if _, err := c.Drv.ForPlayer(xuid, tag); err != nil { // makes sure the requesting player has one (profile-based if needed)
		return nil, err
	}
	n := req.Int("numberOfDrivatars")
	recs, err := c.Drv.ForCache(int(n), xuid)
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  ForCache numberOfDrivatars=%d: serving %d", n, len(recs)))
	out := xls.NewStruct()
	out.Set("drivatarInfos", c.recStructs(recs))
	return out, nil
}

func drvForRace(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, tag := r.Xuid, c.Tag(r.Xuid)
	me, err := c.Drv.ForPlayer(xuid, tag)
	if err != nil {
		return nil, err
	}
	if me == nil {
		return nil, fmt.Errorf("no Drivatar template for %#x", xuid)
	}
	// Free Roam and Car Meets send bucket 0 (no car in play): keep the player's own value there and draw the grid from the whole shipped range, or
	// every name tag shows the lowest level.
	bucket := req.Int("bucketId")
	inCar := bucket > 0
	if inCar {
		w := c.Drv.BucketWristband(bucket) // the car the player is driving now
		me.Set("wristbandLevel", w)
		if err := c.Drv.SetWristband(xuid, w); err != nil {
			return nil, err
		}
	}
	var cached []int64
	for _, l := range [][]any{req.List("cachedDrivatars"), req.List("cachedDrivatarsNewCars")} {
		for _, v := range l {
			cached = append(cached, xls.AsInt(v))
		}
	}
	var skill *int64
	if s := req.Int("skillSelection"); 0 <= s && s <= 4 {
		skill = &s
	}
	var near *int64
	if inCar {
		w := recInt(me, "wristbandLevel")
		near = &w
	}
	others, err := c.Drv.Pool(int(max(0, req.Int("numberOfDrivatars"))), skill, near, xuid, cached, true)
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  ForRace bucket=%d n=%d skill=%d cached=%d+%d: me + %d others", bucket, req.Int("numberOfDrivatars"), req.Int("skillSelection"),
		len(req.List("cachedDrivatars")), len(req.List("cachedDrivatarsNewCars")), len(others)))
	out := xls.NewStruct()
	out.Set("playerData", jsonToStruct(pyjson.Obj(me)))
	out.Set("drivatarInfos", c.recStructs(others))
	out.Set("currentTime", time.Now().UTC().Format("2006-01-02T15:04:05Z"))
	return out, nil
}

// ---------------------------------------------------------------- permissions and economy

// permissionsTable enables every feature. The client looks features up by methodId: a feature missing from the table reads as 4 ("locked", e.g. the
// "server not available" popup when downloading a gifted car), an entry with access 0 reads as allowed.
func permissionsTable(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	var asked int64
	if req != nil {
		asked = req.Int("maxPermissions")
	}
	n := int(min(asked, 255))
	if n == 0 {
		n = 255
	}
	askedS := "None"
	if req != nil {
		askedS = fmt.Sprint(asked)
	}
	c.Log(r, fmt.Sprintf("  GetPermissionsTable max=%s: enabling features 1..%d", askedS, n))
	out := xls.NewStruct()
	out.Set("permissions", c.allPermissions(n))
	out.Set("featureAreasBanMask", int64(0))
	out.Set("userFlags", int64(0))
	return out, nil
}

// economySettings: credit/XP rates for ForzaEconomyModule. The game has its own rates (copied into the economy module at profile load) and what we
// send overwrites them; the values below are guesses, so by default the game keeps its own (offline play is stock). FH2MP_ECONOMY=guess sends them
// again (online XP scale included).
func economySettings(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	mode := os.Getenv("FH2MP_ECONOMY")
	if mode == "" {
		mode = "game"
	}
	if mode != "guess" {
		return nil, &XlsError{"the game's own economy values apply", 1}
	}
	online := int64(roundHalfEven(100 * xpaudit.XPScale))
	res := c.defCall("ForzaUserGetEconomySettings_Results")
	s := res.Sub("settings")
	s.Set("creditDestinationBonus", int64(2000))
	s.Set("creditDestinationFalloff", int64(0))
	s.Set("xpPerKmOffline", int64(100))
	s.Set("xpPerKmOnline", online)
	s.Set("creditPerKm", int64(1000))
	s.Set("minimumFinishLineXp", int64(100))
	s.Set("tombolaPrizeScale", 1.0)
	s.Set("promoBonusScale", 1.0)
	s.Set("wagerRaceCost", int64(1000))
	s.Set("enableTokens", false)
	s.Set("enableXboxMusic", false)
	c.Log(r, fmt.Sprintf("  GetEconomySettings -> %d XP per km online, 100 offline", online))
	return res, nil
}

func roundHalfEven(x float64) float64 {
	r := float64(int64(x))
	d := x - r
	switch {
	case d > 0.5 || (d == 0.5 && int64(r)%2 != 0):
		r++
	case d < -0.5 || (d == -0.5 && int64(r)%2 != 0):
		r--
	}
	return r
}

// ---------------------------------------------------------------- gifting (Message Center category 5)

// gift is the unclaimed gift notification with this GiftId, for the requesting player.
func (c *Core) gift(r *Req, giftID int64, types ...int64) (id int64, params pyjson.Obj, carID *int64, err error) {
	vis, err := c.Notif.Visible(r.Xuid)
	if err != nil {
		return 0, nil, nil, err
	}
	for _, n := range vis {
		match := false
		for _, t := range types {
			if n.Type == t {
				match = true
			}
		}
		if g, _ := n.Params.Get("GiftId"); match && numEq(g, giftID) {
			if n.Claimed {
				return 0, nil, nil, xerr("gift already retrieved")
			}
			return n.ID, n.Params, n.CarID, nil
		}
	}
	return 0, nil, nil, xerr("no such gift")
}

// claim is the atomic exactly-once claim for the requesting player; the check in gift() is only the fast path.
func (c *Core) claim(r *Req, id int64) error {
	ok, err := c.Notif.Claim(r.Xuid, id)
	if err != nil {
		return err
	}
	if !ok {
		return xerr("gift already retrieved")
	}
	return nil
}

func pStr(p pyjson.Obj, k string) string { v, _ := p.Get(k); s, _ := v.(string); return s }

func pInt(p pyjson.Obj, k string) int64 {
	v, _ := p.Get(k)
	switch x := v.(type) {
	case int64:
		return x
	case uint64:
		return int64(x)
	case float64:
		return int64(x)
	}
	return 0
}

func giftCredits(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	giftID := req.Int("giftId")
	id, p, _, err := c.gift(r, giftID, 0x8500)
	if err != nil {
		return nil, err
	}
	if err := c.claim(r, id); err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  RetrieveCreditsGift #%d: %d CR from %s", giftID, pInt(p, "Amount"), pyjson.PyRepr(pStr(p, "SenderGamerTag"))))
	out := xls.NewStruct()
	out.Set("sendingXuid", pInt(p, "SenderXuid"))
	out.Set("creditAmount", pInt(p, "Amount"))
	return out, nil
}

func giftCar(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	giftID := req.Int("giftId")
	id, p, carRef, err := c.gift(r, giftID, 0x8502)
	if err != nil {
		return nil, err
	}
	var carID int64
	if carRef != nil && *carRef != 0 {
		carID = *carRef
	} else {
		carID = pInt(p, "ModelId")
	}
	car, err := c.Cars.CarStruct(carID)
	if err != nil {
		return nil, err
	}
	if car == nil {
		return nil, xerr("unknown carId %d", carID)
	}
	if err := c.claim(r, id); err != nil {
		return nil, err
	}
	res := c.defCall("ForzaGiftingRetrieveCarGift_Results")
	rc := res.Sub("car")
	for _, k := range car.Names {
		rc.Set(k, car.Vals[k])
	}
	gl := res.Sub("giftListing")
	gl.Set("giftId", giftID)
	gl.Set("senderXuid", pInt(p, "SenderXuid"))
	gl.Set("reason", pInt(p, "Reason"))
	res.Sub("carHistory").Sub("base").Set("originalOwner", cut(pStr(p, "SenderGamerTag"), 31))
	c.Log(r, fmt.Sprintf("  RetrieveCarGift #%d: carId %d (make %d, %d) from %s; blob sizes asked %d/%d/%d", giftID, carID, car.Int("make"), car.Int("year"),
		pyjson.PyRepr(pStr(p, "SenderGamerTag")), req.Int("liveryDetailsBufferSize"), req.Int("partsInstalledBufferSize"), req.Int("partsInTrunkBufferSize")))
	return res, nil
}
