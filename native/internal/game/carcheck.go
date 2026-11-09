package game

import (
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	"fh2mp/internal/bits"
	"fh2mp/internal/catalog"
	"fh2mp/internal/diag"
	"fh2mp/internal/gameopts"
	"fh2mp/internal/netv2"
	"fh2mp/internal/parts"
	"fh2mp/internal/playersetup"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/traffic"
)

// DlcCars are car ids that exist in the game but not in the base catalog (data/dlc_cars.txt, env FH2MP_DLC_CARS).
var DlcCars = map[int64]struct{}{}

// StartupProblems are checks that came up without their data; the hub says so once its log is open.
var StartupProblems []string

// DataDir is the folder with the server's data files (dlc_cars.txt, trafficcars.txt, roadtrip.sqlite).
var DataDir = envStr("FH2MP_DATA_DIR", "data")

// SetDataDir points the data files (dlc_cars.txt, trafficcars.txt, roadtrip.sqlite) at a folder.
func SetDataDir(dir string) {
	DataDir = dir
	traffic.DataDir = dir
}

func loadDlcCars() map[int64]struct{} {
	path := os.Getenv("FH2MP_DLC_CARS")
	if path == "" {
		path = DataDir + "/dlc_cars.txt"
	}
	out := map[int64]struct{}{}
	raw, err := os.ReadFile(path)
	if err != nil {
		if !os.IsNotExist(err) {
			StartupProblems = append(StartupProblems, fmt.Sprintf("DLC car list %s (%T: %v): DLC cars count as unknown", path, err, err))
		}
		return out
	}
	for _, line := range strings.Split(string(raw), "\n") {
		line, _, _ = strings.Cut(line, "#")
		for _, x := range strings.Fields(line) {
			if n, err := strconv.ParseInt(x, 10, 64); err == nil {
				out[n] = struct{}{}
			}
		}
	}
	return out
}

func (w *World) cat() *catalog.Catalog { return w.Cat() }

func i64p(v int64) *int64 { return &v }

// carModelID is car_model_id: the ModelId of a car object update (37 bits: 5-bit variable id 0 then the 32-bit value).
func carModelID(size int, payload bits.Int) (int64, bool) {
	if size == 37 && payload.Field(0, 5) == 0 {
		v := payload.Field(5, 32)
		return int64(int32(uint32(v))), true
	}
	return 0, false
}

// carPosition is car_position: the first 96 bits of a CarPredictionFull body are three float32.
func carPosition(rest bits.Int) [3]float64 {
	var pos [3]float64
	for i := 0; i < 3; i++ {
		pos[i] = float64(math.Float32frombits(uint32(rest.Field(32*i, 32))))
	}
	return pos
}

func (w *World) peerXuid(p *Peer) string { return fmt.Sprintf("%#x", p.Xuid) }

// CustomCarID: car is one the game data does not have (a car pack's own car, not a DLC car or a car players cannot get).
func (w *World) CustomCarID(car int64) bool {
	c := w.cat()
	_, blocked := c.Blocked[int(car)]
	_, known := c.KnownCars[int(car)]
	_, dlc := DlcCars[car]
	return !blocked && len(c.KnownCars) > 0 && !known && !dlc
}

// standinSetup turns a custom car's setup into the stand-in car's complete stock setup: its stock parts, stock tune, no wheel style, livery or
// paint of the custom car (playersetup.StockCorrection, the server's repair of a car with foreign parts) and the stand-in as Model. The custom
// car's own paint and livery ids (e.g. colour 9024000) exist for no stock car, and a remote car built with them is not drawn at all.
func (w *World) standinSetup(peer *Peer, car int64, values map[int]bits.Int) (int, bits.Int, bool) {
	s := w.standinFor(peer, car)
	stock, ok := parts.StockList(w.cat().Parts, s)
	if !ok {
		return 0, nil, false
	}
	out := make(map[int]bits.Int, len(values)+4)
	for k, v := range values {
		out[k] = v
	}
	for k, v := range w.stockCorrection(stock, s) {
		out[k] = v
	}
	if _, has := values[playersetup.Model]; has {
		out[playersetup.Model] = bits.FromUint64(uint64(s))
	}
	size, payload, err := encodeValues(out)
	if err != nil {
		return 0, nil, false
	}
	return size, payload, true
}

// standinModel writes the stand-in car into the setup's Model value (var 103) when it has one: the others' games pick the car to draw from the
// model, not from the part list, and a model they do not have becomes the game's placeholder car (1215 NUL_Car_00, drawn as a VW Bora).
func (w *World) standinModel(peer *Peer, car int64, decoded map[int]playersetup.Field, payload bits.Int) bits.Int {
	f, ok := decoded[playersetup.Model]
	if !ok || UnknownCarPolicy != "standin" {
		return payload
	}
	s := w.standinFor(peer, car)
	if f.Width < 64 && uint64(s) >= 1<<uint(f.Width) {
		return payload
	}
	return replaceBits(payload, f.Offset, f.Width, bits.FromUint64(uint64(s)))
}

// CustomCar: the peer drives a car the game data does not have (a car pack's own car, not a DLC car). It may drive in Free Roam, where the others
// see the stand-in car, but it never races: events and Road Trip refuse it (EligibleForEvent).
func (w *World) CustomCar(peer *Peer) bool {
	return peer.ModelID != nil && w.CustomCarID(*peer.ModelID)
}

// CarModelSeen: the client told us its car. True when the car object must not be relayed to the others (policy kick).
func (w *World) CarModelSeen(peer *Peer, mid int64) bool {
	peer.ModelID = i64p(mid)
	c := w.cat()
	name, blocked := c.Blocked[int(mid)]
	if peer.ModelLogged == nil || *peer.ModelLogged != mid {
		peer.ModelLogged = i64p(mid)
		extra := ""
		if blocked {
			extra = fmt.Sprintf(" (%s: not obtainable in the game)", name)
		}
		w.Logf("%d car model %d%s", peer.Addr.Port, mid, extra)
		det := fmt.Sprintf("model %d", mid)
		if blocked {
			det += " BLOCKED"
		}
		w.note("car", peer.Owner, det)
	}
	_, known := c.KnownCars[int(mid)]
	_, dlc := DlcCars[mid]
	unknown := !blocked && len(c.KnownCars) > 0 && !known && !dlc
	if unknown && UnknownCarPolicy != "off" && !peer.Flagged("unknown") {
		peer.Flag("unknown")
		tag := ""
		if UnknownCarPolicy != "kick" {
			tag = " (log only)"
		}
		tail := ""
		if UnknownCarPolicy == "kick" {
			tail = ": ending the connection"
		}
		w.Logf("CAR POLICY%s: owner %d xuid %#x uses car id %d, which is not in the game data (DLC cars are listed in data/dlc_cars.txt)%s",
			tag, peer.Owner, peer.Xuid, mid, tail)
		if UnknownCarPolicy == "kick" && peer.KickReason == nil {
			r := fmt.Sprintf("car id %d is not in the game", mid)
			peer.leaving(&r)
		}
	}
	if unknown && UnknownCarPolicy == "kick" {
		return true
	}
	if !blocked || CarPolicy == "off" {
		return false
	}
	if CarPolicy == "kick" {
		if peer.KickReason == nil {
			w.Logf("CAR POLICY: owner %d xuid %#x uses car %d (%s), which players cannot get: ending the connection", peer.Owner, peer.Xuid, mid, name)
			r := fmt.Sprintf("car %d (%s)", mid, name)
			peer.leaving(&r)
		}
		return true
	}
	if CarPolicy == "events" {
		if k := fmt.Sprintf("blocked-car:%d", mid); !peer.Flagged(k) {
			peer.Flag(k)
			w.Logf("CAR POLICY (events): owner %d xuid %#x uses car %d (%s), which players cannot get: it drives in Free Roam, events and Road Trip refuse it",
				peer.Owner, peer.Xuid, mid, name)
		}
		return false
	}
	w.Logf("CAR POLICY (log only): owner %d xuid %#x uses car %d (%s), which players cannot get", peer.Owner, peer.Xuid, mid, name)
	return false
}

// BlockedCar: the peer drives a car players cannot get (a traffic-only car), which events and Road Trip refuse under CarPolicy "events".
func (w *World) BlockedCar(peer *Peer) bool {
	if peer.ModelID == nil || CarPolicy != "events" {
		return false
	}
	_, blocked := w.cat().Blocked[int(*peer.ModelID)]
	return blocked
}

// NotForEvents: the peer's car may drive in Free Roam but not race: a car pack's own car or a car players cannot get.
func (w *World) NotForEvents(peer *Peer) bool { return w.CustomCar(peer) || w.BlockedCar(peer) }

func dist3(a, b [3]float64) float64 {
	dx, dy, dz := a[0]-b[0], a[1]-b[1], a[2]-b[2]
	return math.Sqrt(dx*dx + dy*dy + dz*dz)
}

// TrackSpeed measures the car's speed from its relayed positions, about once a second. stamp/scene nil when the prediction was cut short.
func (w *World) TrackSpeed(peer *Peer, pos [3]float64, stamp *uint64, scene *int64) {
	now := w.Mono()
	ref := peer.SpeedRef
	sc := int64(-1)
	if scene != nil {
		sc = *scene
	}
	if ref == nil || now-ref.At > 5.0 || sc != ref.Scene {
		r := &SpeedRef{At: now, Pos: pos, Scene: sc}
		if stamp != nil {
			r.Stamp, r.HasStamp = *stamp, true
		}
		peer.SpeedRef, peer.HardS, peer.SoftS = r, 0, 0
		return
	}
	wall := now - ref.At
	dt := wall
	if stamp != nil && ref.HasStamp {
		d := ((int64(*stamp) - int64(ref.Stamp) + 0x80000000) & 0xFFFFFFFF) - 0x80000000
		dt = float64(d) / 1000.0
		if !(0.5*wall <= dt && dt <= 2.0*wall) {
			if wall >= 1.0 {
				r := &SpeedRef{At: now, Pos: pos, Stamp: *stamp, HasStamp: true, Scene: sc}
				peer.SpeedRef = r
			}
			return
		}
	}
	if dt < 1.0 {
		return
	}
	nr := &SpeedRef{At: now, Pos: pos, Scene: sc}
	if stamp != nil {
		nr.Stamp, nr.HasStamp = *stamp, true
	}
	peer.SpeedRef = nr
	d := dist3(pos, ref.Pos)
	kmh := 3.6 * d / dt
	if math.IsNaN(kmh) || math.IsInf(kmh, 0) || kmh > TeleportKmh {
		peer.HardS, peer.SoftS = 0, 0
		return
	}
	peer.TopKmh = math.Max(peer.TopKmh, kmh)
	climb := 3.6 * (pos[1] - ref.Pos[1]) / dt
	peer.TopClimb = math.Max(peer.TopClimb, climb)
	if climb > FlightClimbKmh {
		peer.ClimbS += dt
	} else {
		peer.ClimbS = 0
	}
	if peer.ClimbS >= FlightSeconds && !peer.Flagged("flight") {
		peer.Flag("flight")
		tail := ""
		if SpeedPolicy == "kick" {
			tail = ": ending the connection"
		}
		w.Logf("FLIGHT: owner %d xuid %#x car %s climbing at %.0f km/h for %.0f s, now at height %.0f%s",
			peer.Owner, peer.Xuid, modelStr(peer), climb, peer.ClimbS, pos[1], tail)
		if SpeedPolicy == "kick" && peer.KickReason == nil {
			r := fmt.Sprintf("flying (%.0f km/h climb)", climb)
			peer.leaving(&r)
		}
	}
	if CarCapture {
		w.Logf("%d CAPTURE speed %.1f km/h over %.2f s, moved %.1f units, car %s, pos (%.1f,%.1f,%.1f)", peer.Addr.Port, kmh, dt, d, modelStr(peer), pos[0], pos[1], pos[2])
	}
	var stock int
	haveStock := false
	if peer.ModelID != nil {
		stock, haveStock = w.cat().TopSpeed[int(*peer.ModelID)]
	}
	soft := 0.0
	if haveStock && stock != 0 {
		soft = math.Min(HardCapKmh, math.Max(float64(stock)*SoftFactor, float64(stock)+SoftExtraKmh))
	}
	if kmh > HardCapKmh {
		peer.HardS += dt
	} else {
		peer.HardS = 0
	}
	if soft != 0 && kmh > soft {
		peer.SoftS += dt
	} else {
		peer.SoftS = 0
	}
	if peer.HardS >= SpeedSeconds && !peer.Flagged("hard") {
		peer.Flag("hard")
		tail := ""
		if SpeedPolicy == "kick" {
			tail = ": ending the connection"
		}
		w.Logf("SPEED: owner %d xuid %#x car %s drove %.0f km/h for %.0f s (hard cap %.0f)%s", peer.Owner, peer.Xuid, modelStr(peer), kmh, peer.HardS, HardCapKmh, tail)
		if SpeedPolicy == "kick" && peer.KickReason == nil {
			r := fmt.Sprintf("speed %.0f km/h", kmh)
			peer.leaving(&r)
		}
	}
	if peer.SoftS >= SpeedSeconds && !peer.Flagged("soft") {
		peer.Flag("soft")
		w.Logf("SPEED (log only): owner %d xuid %#x car %s (stock %d km/h) drove %.0f km/h for %.0f s (soft limit %.0f)", peer.Owner, peer.Xuid, modelStr(peer), stock, kmh, peer.SoftS, soft)
	}
}

func modelStr(p *Peer) string {
	if p.ModelID == nil {
		return "None"
	}
	return strconv.FormatInt(*p.ModelID, 10)
}

// ClockSample logs (DEBUG, sub "clock") the delay and clock error behind this client's car on the other screens.
func (w *World) ClockSample(peer *Peer, echo uint64) {
	pred := peer.CarPrediction
	if pred == nil {
		return
	}
	now := w.NowMs()
	rtt := (now - int64(echo)) & 0xFFFFFFFF
	stamp := int64(pred.Body.Rsh(netv2.CarPredictionBodyBits - 32).Uint64())
	lag := ((now - stamp + 0x80000000) & 0xFFFFFFFF) - 0x80000000
	if rtt >= 5000 || !(-5000 < lag && lag < 5000) {
		return
	}
	peer.ClockSamples = append(peer.ClockSamples, [2]int{int(rtt), int(lag)})
	if len(peer.ClockSamples) < ClockSamples {
		return
	}
	rtts := make([]int, len(peer.ClockSamples))
	lags := make([]int, len(peer.ClockSamples))
	for i, s := range peer.ClockSamples {
		rtts[i], lags[i] = s[0], s[1]
	}
	sort.Ints(rtts)
	sort.Ints(lags)
	peer.ClockSamples = nil
	w.Debugf("clock", "%d owner %d: rtt min %d median %d ms; car stamp lag min %d median %d ms; client clock %+.0f ms against the server's (over %d pings; others extrapolate this car by about its lag + their rtt - their lag + 120 ms)",
		peer.Addr.Port, peer.Owner, rtts[0], rtts[len(rtts)/2], lags[0], lags[len(lags)/2], float64(rtts[0])/2-float64(lags[0]), len(rtts))
}

// TrackSpin is log only: the car's angular velocity against the game's own clamp.
func (w *World) TrackSpin(peer *Peer, rest bits.Int) {
	if peer.Flagged("spin") {
		return
	}
	spin := 0.0
	for i := 0; i < 3; i++ {
		v := int(rest.Field(netv2.CarSpinOffset+16*i, 16))
		if v&0x8000 != 0 {
			v -= 0x10000
		}
		if a := math.Abs(float64(v)); a > spin {
			spin = a
		}
	}
	spin = spin * 10.0 / 0x7FFF
	if spin > SpinLimit+0.5 {
		peer.Flag("spin")
		w.Logf("SPIN (log only): owner %d xuid %#x car %s: %.1f rad/s on one axis (game clamp %.0f)", peer.Owner, peer.Xuid, modelStr(peer), spin, SpinLimit)
	}
}

func replaceBits(payload bits.Int, at, width int, value bits.Int) bits.Int {
	mask := bits.FromUint64(1).Lsh(width)
	mask = subOne(mask).Lsh(at)
	return andNot(payload, mask).Or(value.Lsh(at))
}

func subOne(x bits.Int) bits.Int {
	out := make([]uint64, len(x))
	copy(out, x)
	for i := range out {
		if out[i] != 0 {
			out[i]--
			break
		}
		out[i] = ^uint64(0)
	}
	n := len(out)
	for n > 0 && out[n-1] == 0 {
		n--
	}
	return bits.Int(out[:n])
}

// CheckParts decodes before validating; returns (violation, withhold, size, payload).
func (w *World) CheckParts(peer *Peer, size int, payload bits.Int, oid int) (bool, bool, int, bits.Int) {
	w.standinApplied = false
	if PartPolicy == "off" {
		return false, false, size, payload
	}
	decoded, err := playersetup.Fields(size, payload)
	if err != nil {
		peer.CarDoubtAt = w.Mono()
		if peer.PartNotes < 3 {
			peer.PartNotes++
			tail := ""
			if peer.PartNotes >= 3 {
				tail = "; further ones are not logged"
			}
			w.Warnf("%d PART CHECK: withheld undecodable player object: %v (owner %d xuid %#x, %d bits; the others never see this update%s)",
				peer.Addr.Port, err, peer.Owner, peer.Xuid, size, tail)
		}
		return true, true, size, payload
	}
	if w.Hub != nil {
		w.Hub.PollCatalog()
	}
	values := map[int]bits.Int{}
	for vid, f := range decoded {
		values[vid] = f.Value
	}
	c := w.cat()
	tables := c.Parts
	var repair map[int]bits.Int
	var words []uint32
	if v, ok := values[playersetup.Parts]; ok {
		words = playersetup.PartWords(v)
	}
	var car int64
	haveCar := false
	if words != nil {
		car, haveCar = int64(words[2])/1000, true
	} else if m, ok := values[playersetup.Model]; ok {
		car, haveCar = int64(m.Uint64()), true
	}
	if words != nil {
		peer.CarSeenAt = w.Mono()
		if _, ok := tables.TableIDs["CarBody"][words[2]]; !ok {
			bad, hide, np := w.UnknownCar(peer, payload, decoded[playersetup.Parts].Offset, words)
			if !hide && UnknownCarPolicy == "standin" {
				w.standinApplied = true
				// a stand-in the launcher named is a real car: it gets a complete stock setup. The default stand-in is the game's placeholder car,
				// which draws whatever the setup says (the recorded traces keep that path)
				if w.standinFor(peer, car) != StandinCar {
					if n2, p2, ok := w.standinSetup(peer, car, values); ok {
						return bad, hide, n2, p2
					}
				}
				np = w.standinModel(peer, car, decoded, np)
			}
			return bad, hide, size, np
		}
		if m, ok := values[playersetup.Model]; ok && int64(m.Uint64()) != car {
			w.Warnf("%d PART CHECK: model/body mismatch withheld (owner %d xuid %#x: Model %d, car from the part list %d, body %d)",
				peer.Addr.Port, peer.Owner, peer.Xuid, m.Uint64(), car, words[2])
			return true, true, size, payload
		}
		if peer.PartsRepairCar == nil || car != *peer.PartsRepairCar {
			peer.PartsRepairCar = nil
		}
		hide := w.CarModelSeen(peer, car)
		if hide || peer.KickReason != nil {
			return true, true, size, payload
		}
		errs := parts.IntegrityErrors(tables, words)
		if len(errs) > 0 {
			peer.CarDoubtAt = peer.CarSeenAt
			key := fmt.Sprint(words, PartPolicy)
			if peer.PartLogged != key {
				peer.PartLogged = key
				var rows []string
				for i, e := range errs {
					if i >= 8 {
						break
					}
					rows = append(rows, fmt.Sprintf("slot %d = %d: %s", e.Slot, e.Word, e.Text))
				}
				w.Logf("PART CHECK (%s): owner %d xuid %#x car %d: %s", PartPolicy, peer.Owner, peer.Xuid, car, strings.Join(rows, "; "))
				w.note("parts", peer.Owner, fmt.Sprintf("car %d: invalid setup", car))
				var slots []any
				for i, e := range errs {
					if i >= 8 {
						break
					}
					slots = append(slots, []any{e.Slot, e.Word})
				}
				w.ev("parts_repair", pyjson.Pair{K: "xuid", V: fmt.Sprintf("%#x", peer.Xuid)}, pyjson.Pair{K: "car", V: car},
					pyjson.Pair{K: "slots", V: slots}, pyjson.Pair{K: "policy", V: PartPolicy})
			}
			if PartPolicy == "kick" || PartPolicy == "fix+kick" {
				r := fmt.Sprintf("invalid parts for car %d (slot %d)", car, errs[0].Slot)
				peer.leaving(&r)
			}
			if PartPolicy == "fix" || PartPolicy == "fix+kick" {
				stock, ok := parts.StockList(tables, car)
				if !ok || len(parts.IntegrityErrors(tables, stock)) > 0 {
					why := "its stock part list fails the integrity check itself"
					if !ok {
						why = "the catalog has no stock part list for it"
					}
					w.LogEvery([2]any{"no-stock", car}, 60.0, fmt.Sprintf("PART CHECK (%s): cannot repair car %d (owner %d xuid %#x): %s; the object is withheld",
						PartPolicy, car, peer.Owner, peer.Xuid, why), diag.Warn)
					return true, true, size, payload
				}
				peer.PartsRepairCar = i64p(car)
				repair = w.stockCorrection(stock, car)
				w.NotifyRepair(peer, car)
			} else if PartPolicy == "kick" {
				return true, true, size, payload
			} else {
				return true, false, size, payload
			}
		}
	} else if haveCar && w.CustomCarID(car) {
		// a setup that names only the model (no part list) of a custom car: the others' games look the model up, so it is the stand-in too
		w.standinApplied = UnknownCarPolicy == "standin"
		return true, false, size, w.standinModel(peer, car, decoded, payload)
	} else if haveCar && peer.PartsRepairCar != nil && car != *peer.PartsRepairCar {
		if peer.PartNotes < 3 {
			peer.PartNotes++
			w.Logf("%d PART CHECK: withheld a change from repaired car %d to car %d without a part list (owner %d); the others keep seeing car %d",
				peer.Addr.Port, *peer.PartsRepairCar, car, peer.Owner, *peer.PartsRepairCar)
		}
		return true, true, size, payload
	}
	if peer.PartsRepairCar != nil {
		stock, _ := parts.StockList(tables, *peer.PartsRepairCar)
		correction := w.stockCorrection(stock, *peer.PartsRepairCar)
		attempted := false
		touches := false
		for vid, val := range correction {
			if cur, ok := values[vid]; ok {
				touches = true
				if !cur.Equal(val) {
					attempted = true
				}
			}
		}
		if _, ok := values[playersetup.Model]; ok {
			touches = true
		}
		if repair != nil || (touches && attempted) {
			repair = correction
			for k, v := range correction {
				values[k] = v
			}
			if row, ok := c.Choices[int(*peer.PartsRepairCar)]; ok {
				peer.Choices = &Choices{true, int64(row.Class), row.PI}
			}
			w.CorrectOwnerSetup(peer, oid, correction)
			size, payload, _ = encodeValues(values)
		}
	} else if words != nil {
		payload = w.RentalChoices(peer, car, size, payload)
	}
	return repair != nil, peer.KickReason != nil, size, payload
}

func encodeValues(values map[int]bits.Int) (int, bits.Int, error) { return playersetup.Encode(values) }

func (w *World) stockCorrection(words []uint32, car int64) map[int]bits.Int {
	var ch *struct {
		Class uint64
		PI    float64
	}
	if row, ok := w.cat().Choices[int(car)]; ok {
		ch = &struct {
			Class uint64
			PI    float64
		}{uint64(row.Class), row.PI}
	}
	m, _ := playersetup.StockCorrection(words, ch)
	return m
}

// NotifyRepair tells the player, in the Message Center, that the server replaced their car's parts.
func (w *World) NotifyRepair(peer *Peer, car int64) {
	now := w.Mono()
	last, seen := peer.RepairNotified[car]
	if peer.Xuid == 0 || (seen && now-last < NoticeRepairSeconds) {
		return
	}
	peer.RepairNotified[car] = now
	text := fmt.Sprintf("FH2MP: some parts on your car {car:%d} are not in the server's car database (a car pack or a tuning tool), so other players see it with stock parts.", car)
	if w.Sinks != nil && w.Sinks.Notice != nil {
		go w.Sinks.Notice(peer.Xuid, text, w.Log)
	}
}

// CorrectOwnerSetup sends the normal authority update for the owner's active player object.
func (w *World) CorrectOwnerSetup(peer *Peer, oid int, values map[int]bits.Int) {
	nbits, payload, err := playersetup.Encode(values)
	if err != nil {
		return
	}
	vars := func(b *bits.Writer) { b.Int(payload, nbits) }
	w.Send(peer, true, netv2.ServerUpdate(1, uint64(oid), netv2.RouterBits[1][0], uint64(peer.PlayerSeq), vars))
	peer.PlayerSeq = (peer.PlayerSeq + 1) % 0xFF
	if PartPolicy == "fix+kick" {
		w.FlushPeer(peer)
	}
}

// CorrectOwnerChoices repairs the owner's grid/results too.
func (w *World) CorrectOwnerChoices(peer *Peer, cls uint32, pi float64) {
	nbits, value := parts.ChoicesUpdate(cls, float32(pi))
	vars := func(b *bits.Writer) { b.Int(value, nbits) }
	w.Send(peer, false, netv2.ServerUpdate(1, 0, netv2.RouterBits[1][0], uint64(peer.PlayerSeq), vars))
	peer.PlayerSeq = (peer.PlayerSeq + 1) % 0xFF
}

func (w *World) wantChoices(peer *Peer) (CarRow, bool) {
	if peer.RentalCar == nil {
		return CarRow{}, false
	}
	row, ok := w.cat().Choices[int(*peer.RentalCar)]
	return CarRow{uint32(row.Class), row.PI}, ok
}

// CarRow is a catalog (class, PI).
type CarRow struct {
	Class uint32
	PI    float64
}

// NoteTypedChoices tracks independently dirty class/PI fields in every legal player delta.
func (w *World) NoteTypedChoices(peer *Peer, size int, payload bits.Int) bits.Int {
	values, err := playersetup.Fields(size, payload)
	if err != nil {
		w.LogEvery([3]any{"setup-parse", "choices", peer.Owner}, 60.0,
			fmt.Sprintf("player.setup_unreadable %s: relayed as it came, the class and PI seen for this write stay the old ones",
				diag.KV(diag.KVP{K: "what", V: "car choices"}, diag.KVP{K: "owner", V: peer.Owner}, diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", peer.Xuid)},
					diag.KVP{K: "bits", V: size}, diag.KVP{K: "error", V: err.Error()})), diag.Warn)
		return payload
	}
	_, has81 := values[81]
	_, has82 := values[82]
	if !has81 && !has82 {
		return payload
	}
	var cls Choices
	cls.PI = math.NaN()
	if peer.Choices != nil {
		cls = *peer.Choices
	}
	if f, ok := values[81]; ok {
		cls.HasClass, cls.Class = true, int64(f.Value.Uint64())
	}
	if f, ok := values[82]; ok {
		cls.PI = float64(math.Float32frombits(uint32(f.Value.Uint64())))
	}
	peer.Choices = &Choices{cls.HasClass, cls.Class, cls.PI}
	want, ok := w.wantChoices(peer)
	if ok && cls.HasClass && cls.Class == RentalClassID {
		w.CorrectOwnerChoices(peer, want.Class, want.PI)
		for _, pr := range []struct {
			vid int
			val bits.Int
		}{{81, bits.FromUint64(uint64(want.Class))}, {82, bits.FromUint64(uint64(math.Float32bits(float32(want.PI))))}} {
			if f, ok := values[pr.vid]; ok {
				payload = replaceBits(payload, f.Offset, f.Width, pr.val)
			}
		}
		peer.Choices = &Choices{true, int64(want.Class), want.PI}
	}
	return payload
}

// NoteChoices remembers the class and PI a game sends with its car; a rental that reports class R gets its real class and PI back.
func (w *World) NoteChoices(peer *Peer, size int, payload bits.Int) bits.Int {
	found, ok := parts.FindChoices(size, payload)
	if !ok {
		return payload
	}
	payload, bad := parts.SanitizeChoices(payload, found)
	if len(bad) > 0 && peer.ChoicesNotes < 3 {
		peer.ChoicesNotes++
		w.Logf("%d CHOICES: owner %d xuid %#x sent values the other clients cannot show: %s (replaced)", peer.Addr.Port, peer.Owner, peer.Xuid, strings.Join(bad, "; "))
	}
	clsPtr, pi := parts.ReadChoices(payload, found)
	cur := Choices{PI: pi}
	if clsPtr != nil {
		cur.HasClass, cur.Class = true, *clsPtr
	} else if peer.Choices != nil && peer.Choices.HasClass {
		cur.HasClass, cur.Class = true, peer.Choices.Class
	}
	peer.Choices = &Choices{cur.HasClass, cur.Class, cur.PI}
	want, ok := w.wantChoices(peer)
	if ok && cur.HasClass && cur.Class == RentalClassID && !(cur.Class == int64(want.Class) && roundTo(pi, 3) == roundTo(want.PI, 3)) {
		w.CorrectOwnerChoices(peer, want.Class, want.PI)
		w.Logf("%d RENTAL: owner %d car %d reports class %d PI %.3f, shown to the others as class %d PI %.3f",
			peer.Addr.Port, peer.Owner, *peer.RentalCar, cur.Class, pyfmt.F(pi), want.Class, want.PI)
		payload = parts.WriteChoices(payload, found, want.Class, float32(want.PI))
		peer.Choices = &Choices{true, int64(want.Class), want.PI}
	}
	return payload
}

func roundTo(x float64, n int) float64 {
	p := math.Pow(10, float64(n))
	return math.RoundToEven(x*p) / p
}

// ShortRentalChoices handles the short [81 class][82 PI] update a rental's game sends right after its part list.
func (w *World) ShortRentalChoices(peer *Peer, size int, payload bits.Int) bits.Int {
	found, ok := parts.FindShortChoices(size, payload)
	if !ok {
		return payload
	}
	clsP, piP := parts.ReadShortChoices(payload, found)
	want, haveWant := w.wantChoices(peer)
	if haveWant && ((clsP != nil && *clsP == RentalClassID) || (clsP == nil && piP != nil && *piP >= 0.999)) {
		w.CorrectOwnerChoices(peer, want.Class, want.PI)
		clsS, piS := "None", "None"
		if clsP != nil {
			clsS = strconv.FormatUint(*clsP, 10)
		}
		if piP != nil {
			piS = pyfmt.ReprFloat(*piP)
		}
		w.Logf("%d RENTAL: owner %d car %d sent class %s PI %s, shown as class %d PI %.3f", peer.Addr.Port, peer.Owner, *peer.RentalCar, clsS, piS, want.Class, want.PI)
		payload = parts.WriteShortChoices(payload, found, want.Class, float32(want.PI))
		c := uint64(want.Class)
		p := want.PI
		clsP, piP = &c, &p
	}
	prev := Choices{PI: 0}
	if peer.Choices != nil {
		prev = *peer.Choices
	}
	next := prev
	if clsP != nil {
		next.HasClass, next.Class = true, int64(*clsP)
	}
	if piP != nil {
		next.PI = *piP
	}
	peer.Choices = &next
	return payload
}

// Rented says what a rental's own game reports: class R, or the 999 PI of the class-less short update.
func Rented(c *Choices) bool {
	return (c.HasClass && c.Class == RentalClassID) || (!c.HasClass && c.PI >= 0.999)
}

func choicesRepr(c *Choices) string {
	cls := "None"
	if c.HasClass {
		cls = strconv.FormatInt(c.Class, 10)
	}
	return "(" + cls + ", " + pyfmt.ReprFloat(c.PI) + ")"
}

// RentalChoices: the player's car is known (part list): a rental shows its real class and PI to the others.
func (w *World) RentalChoices(peer *Peer, car int64, size int, payload bits.Int) bits.Int {
	c := w.cat()
	_, isRental := c.Rentals[int(car)]
	_, hasChoices := c.Choices[int(car)]
	if isRental && hasChoices {
		peer.RentalCar = i64p(car)
	} else {
		peer.RentalCar = nil
	}
	if peer.RentalCar == nil {
		return payload
	}
	row := c.Choices[int(car)]
	want := CarRow{uint32(row.Class), row.PI}
	if found, ok := parts.FindChoices(size, payload); ok {
		clsP, pi := parts.ReadChoices(payload, found)
		cur := &Choices{PI: pi}
		if clsP != nil {
			cur.HasClass, cur.Class = true, *clsP
		}
		if !Rented(cur) {
			next := Choices{PI: pi}
			if clsP != nil {
				next.HasClass, next.Class = true, *clsP
			} else if peer.Choices != nil {
				next.HasClass, next.Class = peer.Choices.HasClass, peer.Choices.Class
			}
			peer.Choices = &next
			return payload
		}
		if cur.HasClass && cur.Class == RentalClassID && pi < RentalPI {
			w.Logf("%d class R with PI bits %08x (%.5f), car %d: tamper marker or an unusual rental", peer.Addr.Port, math.Float32bits(float32(pi)), pyfmt.F(pi), car)
		}
		if !(cur.HasClass && cur.Class == int64(want.Class)) || math.Abs(pi-want.PI) > 0.002 {
			w.CorrectOwnerChoices(peer, want.Class, want.PI)
			cs := "None"
			if cur.HasClass {
				cs = strconv.FormatInt(cur.Class, 10)
			}
			w.Logf("%d RENTAL: owner %d car %d sent class %s PI %.3f, shown as class %d PI %.3f", peer.Addr.Port, peer.Owner, car, cs, pyfmt.F(pi), want.Class, want.PI)
			payload = parts.WriteChoices(payload, found, want.Class, float32(want.PI))
		}
		peer.Choices = &Choices{true, int64(want.Class), want.PI}
		return payload
	}
	sent := peer.Choices
	if sent == nil || !Rented(sent) {
		return payload
	}
	if !(sent.HasClass && sent.Class == int64(want.Class)) || math.Abs(sent.PI-want.PI) > 0.002 {
		w.CorrectOwnerChoices(peer, want.Class, want.PI)
		w.Logf("%d RENTAL: owner %d car %d: its game sent %s, the others are told class %d PI %.3f", peer.Addr.Port, peer.Owner, car, choicesRepr(sent), want.Class, want.PI)
		nbits, value := parts.ChoicesUpdate(want.Class, float32(want.PI))
		w.RememberAndRelay(peer, netv2.MsgObjectUpdate, 1, 0, nil, 0, nbits, value)
		peer.Choices = &Choices{true, int64(want.Class), want.PI}
	}
	return payload
}

// UnknownCar handles a car the game data does not have: returns (something was wrong, withhold, payload to relay).
func (w *World) UnknownCar(peer *Peer, payload bits.Int, offset int, words []uint32) (bool, bool, bits.Int) {
	car := int64(words[2]) / 1000
	peer.ModelID = i64p(car)
	peer.CarSeenAt = w.Mono()
	peer.CarDoubtAt = peer.CarSeenAt
	what := "log only"
	switch UnknownCarPolicy {
	case "standin":
		what = fmt.Sprintf("the others see car %d", w.standinFor(peer, car))
	case "kick":
		what = "ending the connection"
	}
	if peer.ModelLogged == nil || *peer.ModelLogged != car {
		peer.ModelLogged = i64p(car)
		w.Logf("CAR POLICY (%s): owner %d xuid %#x uses car %d (body %d), which is not in the game data", what, peer.Owner, peer.Xuid, car, words[2])
		w.note("car", peer.Owner, fmt.Sprintf("unknown model %d", car))
	}
	if UnknownCarPolicy == "kick" {
		if peer.KickReason == nil {
			r := fmt.Sprintf("car %d is not in the game", car)
			peer.leaving(&r)
		}
		return true, true, payload
	}
	if UnknownCarPolicy == "standin" {
		standin := w.standinFor(peer, car)
		stand, ok := parts.StockList(w.cat().Parts, standin)
		if !ok {
			w.LogEvery([2]any{"no-standin", car}, 60.0, fmt.Sprintf("CAR POLICY: car %d is unknown and the stand-in car %d has no stock part list either (owner %d xuid %#x): the object is withheld",
				car, standin, peer.Owner, peer.Xuid), diag.Warn)
			return true, true, payload
		}
		return true, false, parts.WriteParts(payload, offset, stand)
	}
	return UnknownCarPolicy != "off", false, payload
}

// KickForCars ends the connections of players the car policy flagged.
func (w *World) KickForCars() {
	for _, q := range w.Peers.Values() {
		if q.KickReason == nil {
			continue
		}
		reason := *q.KickReason
		q.KickReason = nil
		var how string
		if w.Hub != nil && q.Xuid != 0 {
			how = fmt.Sprintf("cheater pool for %s h, then every connection of the account ended", pyfmt.FmtG(CheaterPoolHours))
		} else {
			how = "no hub or no xuid: only this connection ended"
		}
		w.Warnf("kick: owner %d xuid %#x removed: %s (%s)", q.Owner, q.Xuid, reason, how)
		if w.Hub != nil && q.Xuid != 0 {
			xuid, why := q.Xuid, reason
			if w.Hub.Workers() == 0 {
				w.Hub.Flag(xuid, why)
				w.Hub.Revoke(xuid)
			} else {
				// With workers the hub tables change only while they are idle, so the hub's part waits for that; this world's connections end now,
				// as they do in Python (the rest of this tick must not see the player).
				ended := w.dropAccount(xuid)
				w.Hub.Defer(func() {
					h := w.Hub
					h.Flag(xuid, why)
					h.revokeTables(xuid)
					for _, ow := range h.Worlds.Values() {
						if ow != w {
							ended += ow.dropAccount(xuid)
						}
					}
					h.Log(fmt.Sprintf("control: revoked %#x (%d connection(s) ended)", xuid, ended))
				})
			}
		} else {
			w.Send(q, true, w.Goodbye(q))
			w.DropPeer(q, "car policy: "+reason)
		}
	}
}

var _ = time.Now

// NotifyPlayer sends a Message Center notice, at most once per key every interval seconds. No xuid or no notice sink: nothing is sent.
func (w *World) NotifyPlayer(peer *Peer, key string, interval float64, text string) {
	now := w.Mono()
	if peer.Xuid == 0 || w.Sinks == nil || w.Sinks.Notice == nil {
		return
	}
	if last, seen := peer.NoticedAt[key]; seen && now-last < interval {
		return
	}
	peer.NoticedAt[key] = now
	go w.Sinks.Notice(peer.Xuid, text, w.Log)
}

// classLetter is the class a car shows in the game (0 D ... 6 X); other values print as numbers.
func classLetter(c uint64) string {
	letters := []string{"D", "C", "B", "A", "S", "R", "X"}
	if c < uint64(len(letters)) {
		return letters[c]
	}
	return fmt.Sprint(c)
}

// refusalText tells a player why its car does not fit the event: its own class and PI against the rule.
func refusalText(r gameopts.Rule, c *Choices) string {
	own := "class unknown"
	if c != nil && c.HasClass {
		own = fmt.Sprintf("class %s", classLetter(uint64(c.Class)))
		if !math.IsNaN(c.PI) {
			own += fmt.Sprintf(" %d", int(c.PI*1000+0.5))
		}
	}
	need := "any class"
	if r.Class != 255 {
		need = "class " + classLetter(r.Class)
	}
	if r.Min != 0 || r.Max != 0 {
		need += fmt.Sprintf(" with PI %d-%d", r.Min, r.Max)
	}
	return fmt.Sprintf("FH2MP: you were not let into the event: your car is %s, the event needs %s. Pick a car that fits and try again.", own, need)
}

// MaxStandins caps the custom cars one player can name a stand-in for (control "standins").
const MaxStandins = 64

// parseStandins reads "<custom>:<candidate>/<candidate>...,..." (the launcher's choice, closest stock car first, from the car pack's own data):
// each custom car gets the first candidate the game data has, players can own and has a stock part list. -> the stand-ins, one note per car.
func (h *Hub) parseStandins(field string) (map[int64]int64, map[int64]bool, []string) {
	cat := h.catalogPtr.Load()
	out := map[int64]int64{}
	owned := map[int64]bool{}
	var notes []string
	for i, item := range strings.Split(field, ",") {
		if i >= MaxStandins {
			notes = append(notes, fmt.Sprintf("%d more ignored (limit %d)", len(strings.Split(field, ","))-MaxStandins, MaxStandins))
			break
		}
		custom, list, ok := strings.Cut(item, ":")
		car, err := strconv.ParseInt(custom, 10, 64)
		if !ok || err != nil || car <= 0 {
			notes = append(notes, fmt.Sprintf("%q unreadable", item))
			continue
		}
		owned[car] = true
		chosen := int64(0)
		for _, c := range strings.Split(list, "/") {
			id, err := strconv.ParseInt(c, 10, 64)
			if err != nil || cat == nil {
				continue
			}
			_, known := cat.KnownCars[int(id)]
			_, blocked := cat.Blocked[int(id)]
			if _, stock := parts.StockList(cat.Parts, id); known && !blocked && stock {
				chosen = id
				break
			}
		}
		if chosen == 0 {
			notes = append(notes, fmt.Sprintf("car %d: no usable candidate in %q, the others see car %d", car, list, StandinCar))
			continue
		}
		out[car] = chosen
		notes = append(notes, fmt.Sprintf("car %d -> %d", car, chosen))
	}
	return out, owned, notes
}

// HasCustomCars: the player's launcher listed custom cars (a car pack's own cars).
func (w *World) HasCustomCars(p *Peer) bool {
	return w.Hub != nil && len(w.Hub.CustomCars[p.Xuid]) > 0
}

// RealCarsFor: q may see sender's custom cars as they are, because q's game has every custom car sender's launcher listed (the same car pack).
// Fixed for a pair for the whole session (the lists come at sign-in), so q gets one version of each of sender's objects, never a mix.
func (w *World) RealCarsFor(sender, q *Peer) bool {
	if w.Hub == nil {
		return false
	}
	have := w.Hub.CustomCars[q.Xuid]
	s := w.Hub.CustomCars[sender.Xuid]
	if len(s) == 0 {
		return false
	}
	for car := range s {
		if !have[car] {
			return false
		}
	}
	return true
}

// standinFor is the stock car the others see for a player's custom car: the player's own stand-in when the launcher named one, else StandinCar.
func (w *World) standinFor(peer *Peer, car int64) int64 {
	if w.Hub != nil {
		if s, ok := w.Hub.Standins[peer.Xuid][car]; ok {
			return s
		}
	}
	return StandinCar
}
