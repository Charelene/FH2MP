package game

import (
	"math"

	"fh2mp/internal/bits"
	"fh2mp/internal/netv2"
	"fh2mp/internal/omap"
	"fh2mp/internal/reliable"
	"fh2mp/internal/replay"
)

// Addr is a client address: a UDP (ip, port), a TCP tunnel's peer, or ("bot", serial) for a bot.
type Addr struct {
	IP   string
	Port int
}

// ObjKey is (router, object id).
type ObjKey struct{ Router, ID int }

// OwnerObj is (relay owner id, router, object id).
type OwnerObj struct{ Owner, Router, ID int }

// WorldKey is (lobby type, lobby name).
type WorldKey struct {
	Type int
	Name string
}

// Prediction is a client's latest CarPredictionFull: car object id, body bits, bit count.
type Prediction struct {
	ID   int
	Body bits.Int
	Size int
}

// Choices is (class, PI) a player's game last sent; HasClass false = the class is unknown.
type Choices struct {
	HasClass bool
	Class    int64
	PI       float64
}

// SpeedRef is the reference point of the speed measurement.
type SpeedRef struct {
	At       float64
	Pos      [3]float64
	Stamp    uint64
	HasStamp bool
	Scene    int64
}

// VipHidden is the PlayerFlags value with the VIP bit cleared.
type VipHidden struct{ Width, Value int }

// Peer is the per-client state of one connection (= fh2mp_server.Peer).
type Peer struct {
	Addr Addr

	WxSeq                                 int
	WxReady                               bool
	RtSeq                                 int
	RtStarted                             bool
	ConnectedAt                           float64
	RentalCar                             *int64
	Choices                               *Choices
	ChoicesNotes                          int
	ReadyRetry                            *float64
	CarSeenAt, CarDoubtAt, GoptsWrittenAt float64
	GoptsView, GoptsPending               *GameOptionsView
	EsSeq                                 int
	PartsRepairCar                        *int64
	PlayerSeq                             int
	LastID                                int
	Seen                                  map[int]struct{}
	Connected                             bool
	ClientCounter                         uint64
	LastKeepalive                         float64
	TimeSyncSeq                           int
	ConsoleAcked                          bool
	ConsoleSeq                            int
	Owner                                 int
	Entered                               bool
	Introduced                            bool
	CarsHeld                              bool
	BlOid                                 *int
	BlState                               map[string]float64
	BlStateOrder                          []string
	BlCheckAt                             float64
	BlSlotSeq                             int
	Objs                                  *omap.Map[ObjKey, []replay.Entry]
	HostSeq                               int
	RelayNext                             map[ObjKey]int
	SeqShift                              map[OwnerObj]int
	CompactAt                             map[ObjKey]int
	ConsoleVars                           map[int]bool
	GoptsSeq                              int
	VipHidden                             *VipHidden
	PlayerStatus                          int // latest LobbyPlayer.PlayerStatus this client wrote (PlayerVarStatus); valid when HasPlayerStatus
	HasPlayerStatus                       bool
	GoptsReleased                         bool
	GoptsOwnSeq                           *int
	ConsoleOid                            *int
	LastRx                                float64
	LastTimeSync                          float64
	CarsRelayed                           int
	CarID                                 *int
	CarPrediction                         *Prediction
	CarPredictionStamp                    *uint64
	CarPredictionRefreshable              bool
	ClockSamples                          [][2]int
	BotSource                             *Peer
	BotOffset                             [2]float64
	PrivateUniverse                       *int
	LobbyType                             int
	Xuid                                  uint64
	CmSeq                                 int
	World                                 *WorldKey
	Pos                                   *[3]float64
	PosFlagged                            bool
	UnknownLogged                         map[int]struct{}
	MalformedLogged                       bool
	MalformedCount                        int
	OpenedAt                              float64
	OpenResult                            *int
	LostSeen                              int
	UnmappedLogged                        map[[2]int]struct{}
	ModelID                               *int64
	ModelLogged                           *int64
	MaxY                                  float64
	ClimbS                                float64
	TopClimb                              float64
	CarPayloadLogs                        int
	KickReason                            *string
	PartNotes                             int
	PartLogged                            string
	SpeedRef                              *SpeedRef
	InvisibleLogged                       bool
	Push                                  PushState
	RepairNotified                        map[int64]float64
	NoticedAt                             map[string]float64 // Message Center notices sent to this player by key (NotifyPlayer)
	TopKmh                                float64
	HardS, SoftS                          float64
	SpeedFlagged                          map[string]struct{}
	MaxAbs                                float64
	Outbox                                []netv2.Baked
	Window                                reliable.Window
	RxSeq                                 map[ObjKey]int
	DupObjects, DupRequests               int
	Due                                   float64
	Hold                                  float64
	AckCache                              *[2]int
	State                                 PeerState   // half-open / connected / entered / leaving / removed (peerstate.go)
	Refused                               map[int]int // messages not acted on because of the state, per type

	// attributes Python sets dynamically (getattr / setattr in lighting.py, honk.py and the world)
	Lighting         *LightState
	LightingChanges  int
	LightingLoggedAt float64
	Honk             *HonkState
	HonkLoggedAt     float64
	Carsel           *CarselState
	ClientReports    ClientReports // what this client's shim told the server about why it waits (carsel.go)
	Extra            map[string]any
	TeamID           int
	RtPstate         int
}

// NewPeer is Peer(addr) with Python's defaults.
func NewPeer(addr Addr, now float64) *Peer {
	return &Peer{
		Addr: addr, WxSeq: 1, RtSeq: 1, LastID: netv2.NoID, Seen: map[int]struct{}{}, ConsoleSeq: 1,
		LastKeepalive: now, LastRx: now, LastTimeSync: now, CarPredictionRefreshable: true,
		BlState: map[string]float64{}, Objs: omap.New[ObjKey, []replay.Entry](), HostSeq: 2,
		RelayNext: map[ObjKey]int{}, SeqShift: map[OwnerObj]int{}, CompactAt: map[ObjKey]int{}, ConsoleVars: map[int]bool{},
		CmSeq: 1, UnknownLogged: map[int]struct{}{}, UnmappedLogged: map[[2]int]struct{}{}, MaxY: math.Inf(-1),
		RepairNotified: map[int64]float64{}, NoticedAt: map[string]float64{}, SpeedFlagged: map[string]struct{}{}, RxSeq: map[ObjKey]int{}, Due: math.Inf(1),
		Extra: map[string]any{}, Refused: map[int]int{},
	}
}

// Flagged reports whether a speed/policy report was already made.
func (p *Peer) Flagged(k string) bool { _, ok := p.SpeedFlagged[k]; return ok }

// Flag marks a report as made.
func (p *Peer) Flag(k string) { p.SpeedFlagged[k] = struct{}{} }

// PortNumber is the client's port (traffic and log lines name a player by it).
func (p *Peer) PortNumber() int { return p.Addr.Port }

// CarPos is the last car position, if known.
func (p *Peer) CarPos() ([3]float64, bool) {
	if p.Pos == nil {
		return [3]float64{}, false
	}
	return *p.Pos, true
}

// OwnerID implements bucketlist.Member (the relay owner id).
func (p *Peer) OwnerID() int { return p.Owner }

// NotePacketID records the id of a received reliable packet.
func (p *Peer) NotePacketID(pid int) {
	if pid == netv2.UnreliableID {
		return
	}
	p.AckCache = nil
	if p.LastID == netv2.NoID || ((pid-p.LastID)%0xFFFF+0xFFFF)%0xFFFF < 0x8000 {
		p.LastID = pid
	}
	p.Seen[pid] = struct{}{}
	if len(p.Seen) > 64 {
		keep := map[int]struct{}{}
		for s := range p.Seen {
			if ((p.LastID-s)%0xFFFF+0xFFFF)%0xFFFF <= 40 {
				keep[s] = struct{}{}
			}
		}
		p.Seen = keep
	}
}

// AckFields returns (last id, ack bits).
func (p *Peer) AckFields() (int, uint32) {
	if p.AckCache != nil {
		return p.AckCache[0], uint32(p.AckCache[1])
	}
	if p.LastID == netv2.NoID {
		return netv2.NoID, 0
	}
	var b uint32
	for pid := range p.Seen {
		d := ((p.LastID-pid)%0xFFFF + 0xFFFF) % 0xFFFF
		if d >= 1 && d <= 32 {
			b |= 1 << uint(d-1)
		}
	}
	p.AckCache = &[2]int{p.LastID, int(b)}
	return p.LastID, b
}

// BLState implements bucketlist.Member: the last decoded "State" variable.
func (p *Peer) BLState() (float64, bool) { v, ok := p.BlState["State"]; return v, ok }
