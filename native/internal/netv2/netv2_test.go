package netv2

import (
	"encoding/hex"
	"encoding/json"
	"os"
	"testing"

	"fh2mp/internal/bits"
)

type vec struct {
	Hex   string `json:"hex"`
	In    string `json:"in"`
	Size  int    `json:"size"`
	New   uint64 `json:"new"`
	Out   string `json:"out"`
	Found int    `json:"found"`
}

func load(t *testing.T) map[string]vec {
	t.Helper()
	raw, err := os.ReadFile("../../testdata/netv2.json")
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]vec
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatal(err)
	}
	return m
}

func hexInt(t *testing.T, s string) bits.Int {
	t.Helper()
	if len(s) > 1 && s[:2] == "0x" {
		s = s[2:]
	}
	if len(s)%2 == 1 {
		s = "0" + s
	}
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	for i, j := 0, len(b)-1; i < j; i, j = i+1, j-1 {
		b[i], b[j] = b[j], b[i]
	}
	return bits.FromBytes(b)
}

func i(x int64) *int64   { return &x }
func s(x string) *string { return &x }

func TestPacketsMatchPython(t *testing.T) {
	v := load(t)
	big := hexInt(t, "0x1")
	big = big.Lsh(CarPredictionBodyBits)
	// (1 << 710) - 12345
	sub := bits.FromUint64(12345)
	big = subInt(big, sub)
	wx := &Weather{ACur: 0.25, ADes: 0.6666666666666666, PCur: 0, PDes: 1, ACurTick: 1234567, ADesTick: 99999999999, PCurTick: 5, PDesTick: 7, Presets: [4]int64{0, 1, 3, -1}}
	mins := [3][4]int{{1, 2, 3, 4}, {1, 2, 3, 4}, {1, 2, 3, 4}}
	es := &EventSelection{Locations: [3]int64{3, 4, 5}, Events: [3][4]int64{{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 10001, 12}}, Classes: [3]int{1, 2, 3}, Mins: &mins, Votes: [3]int{1, 2, 3}}
	w4 := func(n uint64, w int) VarsWriter { return func(x *bits.Writer) { x.Bits(n, w) } }
	cases := []struct {
		name     string
		msgs     []Msg
		ack, rem uint16
		ackBits  uint32
	}{
		{"open_response", []Msg{OpenConnectionResponse(1, 0, 0xDEADBEEF, 77)}, 0xFFFF, 0, 0},
		{"pong_ping_sync", []Msg{Pong(5, 6), Ping(9), TimeSync(3, 123456), CloseConnection(2, 7, 8), ConnectionSyncDone(55)}, 3, 4, 0xF0F0},
		{"finalise_request", []Msg{FinaliseSessionEntry(), RequestRaceRelay()}, 0xFFFF, 0, 0},
		{"session_host", []Msg{
			SessionHostCreate(1, func(w *bits.Writer) { w.Bits(4, 4); w.Bits(1, 1) }), SessionHostUpdate(2, w4(9, 4))}, 0xFFFF, 0, 0},
		{"object_msgs", []Msg{
			ObjectMessage(7, 2, 5, 0, 3, 7, 0, w4(0x1234567, 30)),
			RelayObjectMessage(7, 1, 9, 33, 0, 4, 7, 0, 40, bits.FromUint64(0xABCDEF1234)),
			RelayObjectMessage(8, 6, 1, 20, 1, 255, 2, 2, 700, big),
			TransferUpdateMessage(9, 0, 4, 12, 710, big),
			GrantOwnership(6, 1, 2, 3, 12, bits.FromUint64(0xFFF)), GrantOwnership(6, 1, 2, 3, 0, nil),
			TransferOwnership(9, 0, 4, 200), RelayDelete(2, 5, 100, 7, 66),
			ServerUpdate(3, 1, 7, 8, w4(0xAB, 8))}, 0xFFFF, 0, 0},
		{"car_prediction", []Msg{RelayCarPrediction(11, 4, big, CarPredictionBodyBits), RelayScore(44, 3, 9, bits.FromUint64(0xFEEDFACE), 40)}, 0xFFFF, 0, 0},
		{"grid_tag", []Msg{
			StartingGrid([]GridEntry{{true, 3, 0}, {false, 4, 6}, {false, 5, 7}}), StartRace(0x123456789), ReadyRequestResponse(2),
			TagBuckets([]TagEntry{{false, 1, 0, 1}, {true, 2, 5, 0}})}, 0xFFFF, 0, 0},
		{"weather", []Msg{WeatherCreate(1, wx), WeatherUpdate(9, wx)}, 0xFFFF, 0, 0},
		{"road_trip", []Msg{
			RoadTripCreate(1, RTValue{"CurrentLocationId", 3}, RTValue{"State", 1}, RTValue{"StateEndTime", 123456}),
			RoadTripUpdate(4, RTValue{"EventRaceId", 10}, RTValue{"DecreaseXPEnabled", 1}, RTValue{"CompletedEventCount", 2}),
			RoadTripEventSelectionUpdate(5, es), RoadTripVoteCountsUpdate(6, [3]int{3, 0, 5}), RoadTripConfigureGameOptions(5, 0x100000001)}, 0xFFFF, 0, 0},
		{"car_meet", []Msg{
			CarMeetCreate(1, CarMeetVarsWriter(CarMeetFields{InviteType: i(1), Countdown: i(10000), Joins: map[int]int{2: 1, 0: 2}, Session: s("abc"), Template: s("t")})),
			CarMeetUpdate(2, CarMeetVarsWriter(CarMeetFields{WagerEnd: i(5)})), CarMeetNotifyPending(3), CarMeetSessionReady("sess", "tmpl")}, 0xFFFF, 0, 0},
	}
	for _, c := range cases {
		got := hex.EncodeToString(BuildPacket(c.msgs, c.ack, c.rem, c.ackBits))
		if got != v[c.name].Hex {
			t.Errorf("%s:\n got %s\nwant %s", c.name, got, v[c.name].Hex)
		}
		baked := make([]Baked, len(c.msgs))
		for k, m := range c.msgs {
			baked[k] = Bake(m)
		}
		if pk := hex.EncodeToString(PackMessages(baked, c.ack, c.rem, c.ackBits)); pk != v[c.name].Hex {
			t.Errorf("%s PackMessages differs from build_packet", c.name)
		}
	}
}

func subInt(a, b bits.Int) bits.Int {
	out := make([]uint64, len(a))
	var borrow uint64
	for k := range out {
		var bv uint64
		if k < len(b) {
			bv = b[k]
		}
		d, br := sub64(a[k], bv, borrow)
		out[k], borrow = d, br
	}
	n := len(out)
	for n > 0 && out[n-1] == 0 {
		n--
	}
	return bits.Int(out[:n])
}

func sub64(a, b, borrow uint64) (uint64, uint64) {
	d := a - b - borrow
	if a < b || (a == b && borrow == 1) {
		return d, 1
	}
	return d, 0
}

func TestRewriteXuid(t *testing.T) {
	v := load(t)["rewrite_xuid"]
	in := hexInt(t, v.In)
	out, found := RewriteXuid(in, v.Size, v.New)
	if found != v.Found || !out.Equal(hexInt(t, v.Out)) {
		t.Fatalf("found %d want %d", found, v.Found)
	}
}
