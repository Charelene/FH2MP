// Package netv2 is the NetV2 wire layer of the game server: message ids, router tables, the 88-bit datagram header and the builders
// of every server-written message (= the top of server/fh2mp_server.py, lines 127-1265).
package netv2

import (
	"encoding/binary"
	"math"

	"fh2mp/internal/bits"
)

// Message ids and protocol constants.
const (
	VersionString = "6CC152FD" // PTR_s_6CC152FD in the game binary
	GameString    = "Flagship"
	HeaderBits    = 88

	MsgOpenConnection         = 0
	MsgOpenConnectionResponse = 1
	MsgCloseConnection        = 2
	MsgPing                   = 3
	MsgPong                   = 4
	MsgTimeSync               = 5
	MsgConnectionSyncDone     = 6
	FirstObjectMsg            = 7
	ResponseAck               = 6
	MsgServerTrafficMessage   = 71
	MsgClientTrafficMessage   = 72
	MsgFinaliseSessionEntry   = 34

	MsgObjectCreate            = 7
	MsgObjectUpdate            = 8
	MsgObjectDelete            = 9
	MsgObjectRequestOwnership  = 10
	MsgObjectGrantOwnership    = 11
	MsgObjectTransferOwnership = 12
	MsgCarPredictionFull       = 15
	MsgCarCollision            = 21
	MsgInProgressScoreUpdate   = 43
	MsgLapScore                = 44
	MsgScoreKeyframe           = 45
	MsgRequestRace             = 38
	MsgVoteToKick              = 46 // a lobby key and the u64 id of the player to remove (the popup's Kick From Session / Vote To Kick)
	MsgRequestLeaderChange     = 59 // "a request to change the leader to the player specified": a player key (the popup's Make Session Leader)
	MsgDropLeadership          = 60
	MsgReadyRequest            = 41
	MsgStartingGrid            = 39
	MsgStartRace               = 40
	MsgReadyRequestResponse    = 42
	MsgRepairDamage            = 35
	MsgTagCollision            = 36
	MsgTagBuckets              = 37
	MsgVoteForTrack            = 47
	MsgRTSelectedNextEvent     = 49
	MsgRTConfigureGameOptions  = 50
	MsgRTConfiguredGameOptions = 51
	MsgCollObjDetachPart       = 24
	MsgChatManagement          = 33

	MsgCarMeetInviteRequest   = 26
	MsgCarMeetJoinOrCancel    = 27
	MsgCarMeetLaunchWagerRace = 28
	MsgCarMeetNotifyPending   = 29
	MsgCarMeetConfigureWager  = 30
	MsgCarMeetConfiguredWager = 31
	MsgCarMeetSessionReady    = 32

	RouterLobby    = 6
	RouterWeather  = 10
	RouterCarMeet  = 11
	RouterRoadTrip = 9
	RouterCar      = 2
	RouterBucket   = 12

	LobbyIDBits   = 2
	LobbyTypeBits = 2
	LobbyEventKey = 4

	TypeSessionHost = 0
	TypeGameOptions = 1

	WeatherVarIDBits   = 4
	CarMeetVarIDBits   = 5
	ConsoleVarIDBits   = 4
	SessionHostVarBits = 4

	CarPredictionBodyBits = 710
	CarUniverseOffset     = 395
	CarTeleportOffset     = 392
	CarSpinOffset         = 0x12 * 8
	CarKeyIDBits          = 7

	RTVarIDBits   = 4
	RTESVarIDBits = 7
	RTESObjectID  = 1
	RTTypeSynced  = 0
	RTTypeEvSel   = 1
	RTTypePlayer  = 2

	TagSlots = 12

	NoID         = 0xFFFE
	UnreliableID = 0xFFFF

	// XuidPattern is the Xbox user id Azimuth gives every instance.
	XuidPattern = uint64(0x0009000000000000)
)

// RouterBits is (idBits, typeBitsOnCreate) per router.
var RouterBits = map[int][2]int{
	1: {7, 0}, 2: {7, 0}, 3: {7, 0}, 6: {2, 2}, 9: {4, 2}, RouterWeather: {1, 0}, RouterCarMeet: {1, 0}, RouterBucket: {4, 2},
}

// RouterLimits is (maxObjects, numTypes) of the routers clients own objects on.
var RouterLimits = map[int][2]int{1: {0x40, 1}, 2: {0x40, 1}, 3: {0x40, 1}, 9: {13, 3}, 12: {13, 2}}

// RelayRouters are the routers whose client-owned objects other clients must see.
var RelayRouters = []int{1, 2, 3, 9, 12}

// WeatherVars are the WeatherSynced variable ids.
var WeatherVars = map[string]int{
	"atm.CurrentIntensity": 0, "atm.CurrentTick": 1, "atm.DesiredIntensity": 2, "atm.DesiredTick": 3, "atm.DesiredPresetIndex": 4,
	"pre.CurrentIntensity": 5, "pre.CurrentTick": 6, "pre.DesiredIntensity": 7, "pre.DesiredTick": 8,
}

// RTVar is a RoadTripSynced variable: id and width.
type RTVar struct {
	Name  string
	ID    int
	Width int
}

// RTVars in declaration order of fh2mp_server.RT_VARS.
var RTVars = []RTVar{
	{"CurrentLocationId", 0, 32}, {"CompletedEventCount", 1, 3}, {"EventRaceId", 2, 32}, {"NextRaceId", 3, 32},
	{"MinimumDriveToDestinationReward", 4, 14}, {"MaximumDriveToDestinationReward", 5, 14},
	{"MaximumDriveToDestinationRewardEarntBefore", 6, 32}, {"DecreaseXPEnabled", 7, 1}, {"State", 8, 4}, {"StateEndTime", 9, 32},
}

// CMVar is a CarMeetSynced scalar variable.
type CMVar struct{ ID, Width int }

// CMVars are GameModeInviteType, InviteCountDownTime, JoinEndTimeWagerRace, JoinEndTimeGameMode.
var CMVars = map[string]CMVar{"GameModeInviteType": {0, 2}, "InviteCountDownTime": {1, 32}, "JoinEndTimeWagerRace": {2, 32}, "JoinEndTimeGameMode": {3, 32}}

// Car Meet variable ids after the scalar ones.
const (
	CMVarJoin0    = 4
	CMVarSession  = 16
	CMVarTemplate = 17
)

// Msg writes one message.
type Msg func(w *bits.Writer)

// Baked is a message written once: its bits and bit count.
type Baked struct {
	V bits.Int
	N int
}

// Bake writes m once.
func Bake(m Msg) Baked {
	var w bits.Writer
	m(&w)
	return Baked{w.Value(), w.Pos}
}

// Type is the message id of a baked message (low 7 bits).
func (b Baked) Type() int { return int(b.V.Uint64() & 0x7F) }

// Header is the 88-bit datagram header.
type Header struct {
	AckID, RemoteAck uint16
	AckBits          uint32
	Count            uint8
	Size             uint16
}

// ParseHeader reads the header.
func ParseHeader(r *bits.Reader) (Header, error) {
	var h Header
	v, err := r.Bits(16)
	if err != nil {
		return h, err
	}
	h.AckID = uint16(v)
	if v, err = r.Bits(16); err != nil {
		return h, err
	}
	h.RemoteAck = uint16(v)
	if v, err = r.Bits(32); err != nil {
		return h, err
	}
	h.AckBits = uint32(v)
	if v, err = r.Bits(8); err != nil {
		return h, err
	}
	h.Count = uint8(v)
	if v, err = r.Bits(16); err != nil {
		return h, err
	}
	h.Size = uint16(v)
	return h, nil
}

// BuildPacket is build_packet: header plus the messages written in order.
func BuildPacket(msgs []Msg, ackID, remoteAck uint16, ackBits uint32) []byte {
	var body bits.Writer
	for _, m := range msgs {
		m(&body)
	}
	size := (HeaderBits + body.Pos + 7) / 8
	var w bits.Writer
	w.Bits(uint64(ackID), 16)
	w.Bits(uint64(remoteAck), 16)
	w.Bits(uint64(ackBits), 32)
	w.Bits(uint64(len(msgs)), 8)
	w.Bits(uint64(size), 16)
	w.Raw(body.Value(), body.Pos)
	return w.Bytes()
}

// PackMessages is pack_messages: build_packet for baked messages, same bytes.
func PackMessages(baked []Baked, ackID, remoteAck uint16, ackBits uint32) []byte {
	var body bits.Writer
	for _, b := range baked {
		body.Raw(b.V, b.N)
	}
	nbytes := (body.Pos + 7) / 8
	out := make([]byte, 11, 11+nbytes)
	binary.LittleEndian.PutUint16(out[0:], ackID)
	binary.LittleEndian.PutUint16(out[2:], remoteAck)
	binary.LittleEndian.PutUint32(out[4:], ackBits)
	out[8] = byte(len(baked))
	binary.LittleEndian.PutUint16(out[9:], uint16(HeaderBits/8+nbytes))
	return append(out, body.Bytes()...)
}

func f32(x float64) uint64 { return uint64(math.Float32bits(float32(x))) }

// OpenConnectionResponse is open_connection_response.
func OpenConnectionResponse(flag2, result, echo, serverCounter uint64) Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgOpenConnectionResponse, 7)
		w.Bits(flag2, 2)
		w.Bits(result, 3)
		w.Bits(echo, 32)
		w.Bits(serverCounter, 32)
	}
}

// Pong is pong.
func Pong(echo, now uint64) Msg {
	return func(w *bits.Writer) { w.Bits(MsgPong, 7); w.Bits(echo, 32); w.Bits(now, 32) }
}

// TimeSync is time_sync.
func TimeSync(seq, now uint64) Msg {
	return func(w *bits.Writer) { w.Bits(MsgTimeSync, 7); w.Bits(seq, 16); w.Bits(now, 32) }
}

// PingLive stamps the ping when the packet is built.
func PingLive(clock func() uint64) Msg {
	return func(w *bits.Writer) { w.Bits(MsgPing, 7); w.Bits(clock(), 32) }
}

// TimeSyncLive stamps the TimeSync when the packet is built.
func TimeSyncLive(seq uint64, clock func() uint64) Msg {
	return func(w *bits.Writer) { w.Bits(MsgTimeSync, 7); w.Bits(seq, 16); w.Bits(clock(), 32) }
}

// Ping is ping.
func Ping(v uint64) Msg { return func(w *bits.Writer) { w.Bits(MsgPing, 7); w.Bits(v, 32) } }

// RequestRaceRelay is request_race_relay.
func RequestRaceRelay() Msg { return func(w *bits.Writer) { w.Bits(MsgRequestRace, 7) } }

// FinaliseSessionEntry is finalise_session_entry.
func FinaliseSessionEntry() Msg { return func(w *bits.Writer) { w.Bits(MsgFinaliseSessionEntry, 7) } }

// CloseConnection is close_connection.
func CloseConnection(reason, a, b uint64) Msg {
	return func(w *bits.Writer) { w.Bits(MsgCloseConnection, 7); w.Bits(reason, 3); w.Bits(a, 32); w.Bits(b, 32) }
}

// ConnectionSyncDone is connection_sync_done.
func ConnectionSyncDone(v uint64) Msg {
	return func(w *bits.Writer) { w.Bits(MsgConnectionSyncDone, 7); w.Bits(v, 32) }
}

// VarsWriter writes the variable section of an object message.
type VarsWriter func(w *bits.Writer)

// ObjectMessage is object_message: a synced-object create (7) or update (8) with a plain local key.
func ObjectMessage(msgType, router, objID, typeID, seq uint64, idBits, typeBits int, vars VarsWriter) Msg {
	return func(w *bits.Writer) {
		w.Bits(msgType, 7)
		w.Bits(router, 4)
		w.Bits(0, 1)
		w.Bits(0, 1)
		w.Bits(objID, idBits)
		if msgType == MsgObjectCreate && typeBits != 0 {
			w.Bits(typeID, typeBits)
		}
		w.Bits(seq, 8)
		var body bits.Writer
		if vars != nil {
			vars(&body)
		}
		w.Bits(uint64(body.Pos), 14)
		w.Raw(body.Value(), body.Pos)
	}
}

// RelayObjectMessage is relay_object_message: another client's object as relayed (key A=0 B=1, owner after the id).
func RelayObjectMessage(msgType, router, objID, owner, typeID, seq uint64, idBits, typeBits, size int, payload bits.Int) Msg {
	return func(w *bits.Writer) {
		w.Bits(msgType, 7)
		w.Bits(router, 4)
		w.Bits(0, 1)
		w.Bits(1, 1)
		w.Bits(objID, idBits)
		w.Bits(owner, 8)
		if msgType == MsgObjectCreate && typeBits != 0 {
			w.Bits(typeID, typeBits)
		}
		w.Bits(seq, 8)
		w.Bits(uint64(size), 14)
		w.Raw(payload, size)
	}
}

// TransferUpdateMessage is transfer_update_message (key A=1 B=0).
func TransferUpdateMessage(router, objID uint64, idBits int, seq uint64, size int, payload bits.Int) Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgObjectUpdate, 7)
		w.Bits(router, 4)
		w.Bits(1, 1)
		w.Bits(0, 1)
		w.Bits(objID, idBits)
		w.Bits(seq, 8)
		w.Bits(uint64(size), 14)
		w.Raw(payload, size)
	}
}

// GrantOwnership is grant_ownership.
func GrantOwnership(router, objID uint64, idBits int, seq uint64, size int, payload bits.Int) Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgObjectGrantOwnership, 7)
		w.Bits(router, 4)
		w.Bits(1, 1)
		w.Bits(0, 1)
		w.Bits(objID, idBits)
		w.Bits(seq, 8)
		w.Bits(uint64(size), 14)
		if size != 0 {
			w.Raw(payload, size)
		}
	}
}

// TransferOwnership is transfer_ownership (message 12).
func TransferOwnership(router, objID uint64, idBits int, seq uint64) Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgObjectTransferOwnership, 7)
		w.Bits(router, 4)
		w.Bits(1, 1)
		w.Bits(0, 1)
		w.Bits(objID, idBits)
		w.Bits(seq, 8)
	}
}

// GridEntry is one starting-grid slot: Local (the receiver's own car) or relay with an owner.
type GridEntry struct {
	Local bool
	ID    uint64
	Owner uint64
}

// StartingGrid is starting_grid; at most 12 entries.
func StartingGrid(entries []GridEntry) Msg {
	if len(entries) > 12 {
		entries = entries[:12]
	}
	return func(w *bits.Writer) {
		w.Bits(MsgStartingGrid, 7)
		w.Bits(0, LobbyEventKey)
		w.Bits(uint64(len(entries)), 4)
		for _, e := range entries {
			if e.Local {
				w.Bits(0, 1)
				w.Bits(0, 1)
				w.Bits(e.ID, CarKeyIDBits)
			} else {
				w.Bits(0, 1)
				w.Bits(1, 1)
				w.Bits(e.ID, CarKeyIDBits)
				w.Bits(e.Owner, 8)
			}
		}
	}
}

// StartRace is start_race.
func StartRace(startMs uint64) Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgStartRace, 7)
		w.Bits(0, LobbyEventKey)
		w.Bits(startMs&0xFFFFFFFF, 32)
	}
}

// WriteCarKey is write_car_key.
func WriteCarKey(w *bits.Writer, relay bool, id, owner uint64) {
	if relay {
		w.Bits(0, 1)
		w.Bits(1, 1)
		w.Bits(id, CarKeyIDBits)
		w.Bits(owner, 8)
		return
	}
	w.Bits(0, 2)
	w.Bits(id, CarKeyIDBits)
}

// TagEntry is one MsgTagBuckets slot.
type TagEntry struct {
	Relay  bool
	ID     uint64
	Owner  uint64
	Bucket uint64
}

// TagBuckets is tag_buckets_message: unused slots hold a relay key of owner 0.
func TagBuckets(entries []TagEntry) Msg {
	if len(entries) > TagSlots {
		entries = entries[:TagSlots]
	}
	all := append([]TagEntry{}, entries...)
	for len(all) < TagSlots {
		all = append(all, TagEntry{Relay: true, ID: 127})
	}
	return func(w *bits.Writer) {
		w.Bits(MsgTagBuckets, 7)
		for _, e := range all {
			WriteCarKey(w, e.Relay, e.ID, e.Owner)
		}
		for _, e := range all {
			w.Bits(e.Bucket, 8)
		}
	}
}

// ReadyRequestResponse is ready_request_response.
func ReadyRequestResponse(code uint64) Msg {
	return func(w *bits.Writer) { w.Bits(MsgReadyRequestResponse, 7); w.Bits(0, LobbyEventKey); w.Bits(code, 2) }
}

// SessionHostUpdate is session_host_update.
func SessionHostUpdate(seq uint64, vars VarsWriter) Msg {
	return ObjectMessage(MsgObjectUpdate, RouterLobby, 0, 0, seq, LobbyIDBits, 0, vars)
}

// SessionHostCreate is session_host_create.
func SessionHostCreate(seq uint64, vars VarsWriter) Msg {
	return ObjectMessage(MsgObjectCreate, RouterLobby, 0, TypeSessionHost, seq, LobbyIDBits, LobbyTypeBits, vars)
}

// RelayDelete is relay_delete_message.
func RelayDelete(router, objID, owner uint64, idBits int, seq uint64) Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgObjectDelete, 7)
		w.Bits(router, 4)
		w.Bits(0, 1)
		w.Bits(1, 1)
		w.Bits(objID, idBits)
		w.Bits(owner, 8)
		w.Bits(seq, 8)
	}
}

// RelayScore is relay_score_message.
func RelayScore(msgType, carID, owner uint64, rest bits.Int, restBits int) Msg {
	return func(w *bits.Writer) {
		w.Bits(msgType, 7)
		w.Bits(0, 1)
		w.Bits(1, 1)
		w.Bits(carID, CarKeyIDBits)
		w.Bits(owner, 8)
		w.Raw(rest, restBits)
	}
}

// RelayCarPrediction is relay_car_prediction.
func RelayCarPrediction(objID, owner uint64, rest bits.Int, restBits int) Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgCarPredictionFull, 7)
		w.Bits(0, 1)
		w.Bits(1, 1)
		w.Bits(objID, CarKeyIDBits)
		w.Bits(owner, 8)
		w.Raw(rest, restBits)
	}
}

// ServerUpdate is server_update: both id flags set, id only.
func ServerUpdate(router, objID uint64, idBits int, seq uint64, vars VarsWriter) Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgObjectUpdate, 7)
		w.Bits(router, 4)
		w.Bits(1, 1)
		w.Bits(1, 1)
		w.Bits(objID, idBits)
		w.Bits(seq, 8)
		var body bits.Writer
		if vars != nil {
			vars(&body)
		}
		w.Bits(uint64(body.Pos), 14)
		w.Raw(body.Value(), body.Pos)
	}
}

// Weather is the world's weather state as WeatherSynced carries it.
type Weather struct {
	ACur, ADes, PCur, PDes                 float64
	ACurTick, ADesTick, PCurTick, PDesTick int64
	Presets                                [4]int64
}

// WeatherVarsWriter is weather_vars.
func WeatherVarsWriter(wx *Weather) VarsWriter {
	return func(w *bits.Writer) {
		type row struct {
			name  string
			float bool
			f     float64
			u     int64
		}
		for _, r := range []row{
			{"atm.CurrentIntensity", true, wx.ACur, 0}, {"atm.CurrentTick", false, 0, wx.ACurTick},
			{"atm.DesiredIntensity", true, wx.ADes, 0}, {"atm.DesiredTick", false, 0, wx.ADesTick},
			{"pre.CurrentIntensity", true, wx.PCur, 0}, {"pre.CurrentTick", false, 0, wx.PCurTick},
			{"pre.DesiredIntensity", true, wx.PDes, 0}, {"pre.DesiredTick", false, 0, wx.PDesTick},
		} {
			w.Bits(uint64(WeatherVars[r.name]), WeatherVarIDBits)
			if r.float {
				w.Bits(f32(r.f), 32)
			} else {
				w.Bits(uint64(r.u)&0xFFFFFFFF, 32)
			}
		}
		w.Bits(uint64(WeatherVars["atm.DesiredPresetIndex"]), WeatherVarIDBits)
		for _, idx := range wx.Presets {
			w.Bits(uint64(idx)&0xFFFFFFFF, 32)
		}
	}
}

// WeatherCreate is weather_create.
func WeatherCreate(seq uint64, wx *Weather) Msg {
	return ObjectMessage(MsgObjectCreate, RouterWeather, 0, 0, seq, 1, 0, WeatherVarsWriter(wx))
}

// WeatherUpdate is weather_update.
func WeatherUpdate(seq uint64, wx *Weather) Msg {
	return ObjectMessage(MsgObjectUpdate, RouterWeather, 0, 0, seq, 1, 0, WeatherVarsWriter(wx))
}

// RTValue names one RoadTripSynced variable to write.
type RTValue struct {
	Name  string
	Value int64
}

func rtVar(name string) (RTVar, bool) {
	for _, v := range RTVars {
		if v.Name == name {
			return v, true
		}
	}
	return RTVar{}, false
}

// RoadTripVars is road_trip_vars; values are written in the order given (Python kwargs order).
func RoadTripVars(values ...RTValue) VarsWriter {
	return func(w *bits.Writer) {
		for _, v := range values {
			rv, ok := rtVar(v.Name)
			if !ok {
				panic("unknown Road Trip variable " + v.Name)
			}
			w.Bits(uint64(rv.ID), RTVarIDBits)
			w.Bits(uint64(v.Value), rv.Width)
		}
	}
}

// RoadTripCreate is road_trip_create.
func RoadTripCreate(seq uint64, values ...RTValue) Msg {
	return ObjectMessage(MsgObjectCreate, RouterRoadTrip, 0, RTTypeSynced, seq, 4, 2, RoadTripVars(values...))
}

// RoadTripUpdate is road_trip_update.
func RoadTripUpdate(seq uint64, values ...RTValue) Msg {
	return ObjectMessage(MsgObjectUpdate, RouterRoadTrip, 0, 0, seq, 4, 0, RoadTripVars(values...))
}

// EventSelection is the vote screen's data: 3 locations, 4 events each, a class, minimum players and the vote tally per location.
type EventSelection struct {
	Locations [3]int64
	Events    [3][4]int64
	Classes   [3]int
	Mins      *[3][4]int
	Votes     [3]int
}

// RoadTripEventSelectionVars is road_trip_event_selection_vars.
func RoadTripEventSelectionVars(es *EventSelection) VarsWriter {
	return func(w *bits.Writer) {
		w.Bits(0, RTESVarIDBits)
		for _, loc := range es.Locations {
			w.Bits(uint64(loc)&0xFFFFFFFF, 32)
		}
		for _, first := range []int{1, 31} {
			for b := 0; b < 3; b++ {
				base := first + 10*b
				for k := 0; k < 4; k++ {
					w.Bits(uint64(base+2*k), RTESVarIDBits)
					w.Bits(uint64(es.Events[b][k])&0xFFFFFFFF, 32)
					w.Bits(uint64(base+2*k+1), RTESVarIDBits)
					m := 0
					if es.Mins != nil {
						m = es.Mins[b][k]
					}
					w.Bits(uint64(m), 4)
				}
				w.Bits(uint64(base+8), RTESVarIDBits)
				w.Bits(uint64(es.Classes[b]), 3)
				w.Bits(uint64(base+9), RTESVarIDBits)
				w.Bits(uint64(es.Votes[b]), 5)
			}
		}
	}
}

// RoadTripEventSelectionUpdate is road_trip_event_selection_update.
func RoadTripEventSelectionUpdate(seq uint64, es *EventSelection) Msg {
	var body bits.Writer
	RoadTripEventSelectionVars(es)(&body)
	return TransferUpdateMessage(RouterRoadTrip, 0, 4, seq, body.Pos, body.Value())
}

// RoadTripVoteCountsUpdate is road_trip_vote_counts_update.
func RoadTripVoteCountsUpdate(seq uint64, votes [3]int) Msg {
	var body bits.Writer
	for _, first := range []int{1, 31} {
		for b := 0; b < 3; b++ {
			body.Bits(uint64(first+10*b+9), RTESVarIDBits)
			body.Bits(uint64(votes[b]), 5)
		}
	}
	return TransferUpdateMessage(RouterRoadTrip, 0, 4, seq, body.Pos, body.Value())
}

// RoadTripConfigureGameOptions is road_trip_configure_game_options.
func RoadTripConfigureGameOptions(a, b uint64) Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgRTConfigureGameOptions, 7)
		w.Bits(a&0xFFFFFFFF, 32)
		w.Bits(b&0xFFFFFFFF, 32)
	}
}

// CarMeetCreate is car_meet_create.
func CarMeetCreate(seq uint64, vars VarsWriter) Msg {
	return ObjectMessage(MsgObjectCreate, RouterCarMeet, 0, 0, seq, RouterBits[RouterCarMeet][0], 0, vars)
}

// CarMeetUpdate is car_meet_update.
func CarMeetUpdate(seq uint64, vars VarsWriter) Msg {
	return ObjectMessage(MsgObjectUpdate, RouterCarMeet, 0, 0, seq, RouterBits[RouterCarMeet][0], 0, vars)
}

// CarMeetFields is car_meet_vars' arguments; nil pointers are left out. Joins maps slot -> join type, written by slot.
type CarMeetFields struct {
	InviteType, Countdown, WagerEnd, ModeEnd *int64
	Joins                                    map[int]int
	Session, Template                        *string
}

// CarMeetVarsWriter is car_meet_vars.
func CarMeetVarsWriter(f CarMeetFields) VarsWriter {
	return func(w *bits.Writer) {
		put := func(name string, v *int64) {
			if v == nil {
				return
			}
			c := CMVars[name]
			w.Bits(uint64(c.ID), CarMeetVarIDBits)
			w.Bits(uint64(*v), c.Width)
		}
		put("GameModeInviteType", f.InviteType)
		put("InviteCountDownTime", f.Countdown)
		put("JoinEndTimeWagerRace", f.WagerEnd)
		put("JoinEndTimeGameMode", f.ModeEnd)
		for slot := 0; slot < 64; slot++ {
			if jt, ok := f.Joins[slot]; ok {
				w.Bits(uint64(CMVarJoin0+slot), CarMeetVarIDBits)
				w.Bits(uint64(jt), 2)
			}
		}
		for _, t := range []struct {
			id   int
			text *string
		}{{CMVarSession, f.Session}, {CMVarTemplate, f.Template}} {
			if t.text == nil {
				continue
			}
			runes := []rune(*t.text)
			w.Bits(uint64(t.id), CarMeetVarIDBits)
			w.Bits(uint64(len(runes)), 7)
			if len(runes) > 64 {
				runes = runes[:64]
			}
			for _, ch := range runes {
				w.Bits(uint64(ch), 16)
			}
		}
	}
}

// CarMeetNotifyPending is car_meet_notify_pending.
func CarMeetNotifyPending(action uint64) Msg {
	return func(w *bits.Writer) { w.Bits(MsgCarMeetNotifyPending, 7); w.Bits(action, 2) }
}

// CarMeetSessionReady is car_meet_session_ready.
func CarMeetSessionReady(name, template string) Msg {
	return func(w *bits.Writer) { w.Bits(MsgCarMeetSessionReady, 7); w.String(name); w.String(template) }
}

// RewriteXuid is rewrite_xuid: every 64-bit window equal to XuidPattern is replaced; returns the payload and the replacement count.
func RewriteXuid(payload bits.Int, size int, newValue uint64) (bits.Int, int) {
	found := 0
	for off := 0; off+64 <= size; {
		if payload.Field(off, 64) == XuidPattern {
			mask := bits.FromUint64(^uint64(0)).Lsh(off)
			payload = andNot(payload, mask).Or(bits.FromUint64(newValue).Lsh(off))
			found++
			off += 64
		} else {
			off++
		}
	}
	return payload, found
}

func andNot(x, mask bits.Int) bits.Int {
	out := make(bits.Int, len(x))
	copy(out, x)
	for i := range out {
		if i < len(mask) {
			out[i] &^= mask[i]
		}
	}
	for len(out) > 0 && out[len(out)-1] == 0 {
		out = out[:len(out)-1]
	}
	return out
}

// ReadCarKey is read_car_key: kind is "relay", "own" or "other".
func ReadCarKey(r *bits.Reader) (kind string, id, owner uint64, err error) {
	var a, b uint64
	if a, err = r.Bits(1); err != nil {
		return
	}
	if b, err = r.Bits(1); err != nil {
		return
	}
	if id, err = r.Bits(CarKeyIDBits); err != nil {
		return
	}
	if a == 0 && b == 1 {
		owner, err = r.Bits(8)
		return "relay", id, owner, err
	}
	if a == 0 && b == 0 {
		return "own", id, 0, nil
	}
	return "other", id, 0, nil
}

// UnusedLayout is (key id bits or -1, fixed tail bits) of a message the server parses and ignores.
type UnusedLayout struct{ IDBits, Tail int }

// UnusedMessageLayout is UNUSED_MESSAGE_LAYOUT.
var UnusedMessageLayout = map[int]UnusedLayout{
	20: {7, 352}, MsgCarCollision: {CarKeyIDBits, 192}, MsgRepairDamage: {CarKeyIDBits, 0}, 46: {2, 64}, 52: {2, 32},
	54: {-1, 384}, 56: {-1, 224}, 58: {-1, 96}, 59: {7, 0}, 60: {-1, 0}, 70: {2, 0},
}

// IsUnusedMessage reports membership in UNUSED_MESSAGES.
func IsUnusedMessage(t int) bool {
	if _, ok := UnusedMessageLayout[t]; ok {
		return true
	}
	return t == MsgObjectDelete || t == MsgCollObjDetachPart || t == MsgChatManagement
}

// SkipKey is skip_key.
func SkipKey(r *bits.Reader, idBits int) error {
	a, err := r.Bits(1)
	if err != nil {
		return err
	}
	b, err := r.Bits(1)
	if err != nil {
		return err
	}
	if _, err = r.Bits(idBits); err != nil {
		return err
	}
	if a == 0 && b == 1 {
		_, err = r.Bits(8)
	}
	return err
}

// SkipUnusedMessage is skip_unused_message: false when what follows cannot be trusted. A read error is returned like Python's ValueError.
func SkipUnusedMessage(r *bits.Reader, mtype int) (bool, error) {
	switch mtype {
	case MsgObjectDelete:
		router, err := r.Bits(4)
		if err != nil {
			return false, err
		}
		rb, ok := RouterBits[int(router)]
		if !ok {
			return false, nil
		}
		if err := SkipKey(r, rb[0]); err != nil {
			return false, err
		}
		_, err = r.Bits(8)
		return err == nil, err
	case MsgCollObjDetachPart:
		if _, err := r.Bits(4); err != nil {
			return false, err
		}
		count, err := r.Bits(11)
		if err != nil {
			return false, err
		}
		if count > 1024 {
			return false, nil
		}
		_, err = r.Bits(int(count) * 8)
		return err == nil, err
	case MsgChatManagement:
		if err := r.Skip(64); err != nil {
			return false, err
		}
		n, err := r.Bits(7)
		if err != nil {
			return false, err
		}
		err = r.Skip(int(n) * 8)
		return err == nil, err
	}
	l := UnusedMessageLayout[mtype]
	if l.IDBits >= 0 {
		if err := SkipKey(r, l.IDBits); err != nil {
			return false, err
		}
	}
	if err := r.Skip(l.Tail); err != nil {
		return false, err
	}
	return true, nil
}
