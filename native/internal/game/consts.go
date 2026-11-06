// Package game is the game server engine (= server/fh2mp_server.py): worlds, peers, relay, race flow, Road Trip, Car Meet, the hub that routes
// datagrams to worlds, and its control protocol. It is deterministic given a Clock, a random source and a PacketConn.
package game

import (
	"fmt"
	"os"
	"strconv"
	"strings"

	"fh2mp/internal/capacity"
	"fh2mp/internal/diag"
)

func envStr(name, def string) string {
	if v, ok := os.LookupEnv(name); ok {
		return v
	}
	return def
}

func envFloat(name string, def float64) float64 {
	if v := strings.TrimSpace(os.Getenv(name)); v != "" {
		if f, err := strconv.ParseFloat(v, 64); err == nil {
			return f
		}
	}
	return def
}

func envInt(name string, def int) int {
	if v := strings.TrimSpace(os.Getenv(name)); v != "" {
		if f, err := strconv.ParseInt(v, 0, 64); err == nil {
			return int(f)
		}
	}
	return def
}

// Timing and limits.
const (
	KeepaliveSeconds     = 1.0
	TimeSyncSeconds      = 30.0
	ClockOffsetMaxMs     = 86_400_000
	WorldTickSeconds     = 0.02
	ReplyHoldSeconds     = 0.002
	StaleSeconds         = 20.0
	PendingSeconds       = 15.0
	MaxPendingPeers      = 256
	MaxPendingPerIP      = 8
	RelayOwnerLast       = 127
	RelayHistoryCap      = 8000
	RaceCountdownMs      = 5000
	RaceGridReportSecs   = 10.0
	RaceGridBackstopSecs = 240.0
	RaceInviteSeconds    = 20.0
	RaceJoinGraceSeconds = 45.0
	BlRequestGraceSecs   = 0.8
	RaceOwnRequestSecs   = 3.0
	LeaderSetupSeconds   = 120.0
	EventBlockMaxSeconds = 1200.0
	RaceFlagsClearSecs   = 120.0
	RaceLastFinisherSecs = 30.0
	ProgressSendSeconds  = 1.0
	NoticeRepairSeconds  = 600.0
	ClockSamples         = 30
	SpinLimit            = 6.0
	HardCapKmh           = 451.0
	SoftFactor           = 1.5
	SoftExtraKmh         = 140.0
	TeleportKmh          = 1000.0
	VisibilityCheckSecs  = 10.0
	VisibilityMinPreds   = 300
	SpeedSeconds         = 3.0
	FlightClimbKmh       = 150.0
	FlightSeconds        = 3.0
	PositionLimit        = 50000.0
	PositionWithholdLim  = 1e6
	WipeMinDrops         = 3
	WipeWindowS          = 3.0
	WipeContextS         = 14.0
	RentalClassID        = 7
	RentalPI             = 0.99995
	AuthTTLSeconds       = 600.0
	TicketMagic          = "FH2MPTK1"
	TicketLen            = 84
	TicketSkewSeconds    = 86400.0
	TicketBindSeconds    = 120.0
	MaxTicketNonces      = 500000
	IdleWorldSeconds     = 120.0

	TagSlots           = 12
	TagBucketNotIt     = 0
	TagBucketIt        = 1
	RaceModeKing       = 25
	RaceModeInfection  = 26
	TagKeyframeMinSecs = 20.0
	TagGridAssignDelay = 1.5
	TagResyncS         = 3.0

	LobbyTypeRoadTrip = 1
	LobbyTypeFreeRoam = 2
	LobbyTypeCarMeet  = 3

	ResultOK, ResultVersionMismatch, ResultGameMismatch, ResultBanned = 0, 1, 2, 3
	ResponseAck                                                       = 6
)

// Console / SessionHost / player variable ids.
const (
	ConsoleVarSessionEntryFinalised = 1
	ConsoleVarParticipatingInRace   = 2
	ConsoleVarHasLoadedCars         = 5
	ConsoleVarHasGrid               = 6
	ConsoleVarRenderReady           = 0

	SHRaceReady                = 4
	SHRaceParticipantsSelected = 5
	SHInRace                   = 6
	SHCountdownType            = 2
	SHCountdownTypeBits        = 2
	SHCountdownStartRace       = 2
	SHRaceStartTime            = 9
	SHNumLapsCompleted         = 10
	SHFurthestProgress         = 11

	PlayerVarTeamID          = 90
	PlayerVarIsLeader        = 5
	PlayerVarFlags           = 31
	PlayerFlagVIP            = 1
	PlayerVarPrivateUniverse = 40
	// LobbyPlayer.PlayerStatus (var 19, 4 bits, CPlayerStatus::Enum): 1 CarSelect, 3 PreRace (see the Python server's PLAYER_VAR_STATUS).
	PlayerVarStatus       = 19
	PlayerStatusCarSelect = 1
	PlayerStatusPreRace   = 3
	PrivateUniverseBits   = 5
	PrivateUniverseFirst  = 1
	PrivateUniverseCount  = 17
	LobbyPlayerPlayerHost = 1
)

// ConsoleBoolVarNames names the Console bool variables.
var ConsoleBoolVarNames = map[int]string{0: "RenderReady", 1: "SessionEntryFinalised", 2: "ParticipatingInRace", 3: "RaceInProgress", 4: "Ready",
	5: "HasLoadedCarsForAllParticipants", 6: "HasGrid"}

// Environment-derived settings, read once at start.
var (
	Verbosity        = diag.NewVerbosity(nil, nil)
	RelayFlushSecs   = envFloat("FH2MP_RELAY_FLUSH_MS", 10) / 1000.0
	MaxPacketBytes   = envInt("FH2MP_MAX_PACKET_BYTES", 1000)
	Reliable         = envStr("FH2MP_RELIABLE", "0") == "1"
	OwnerQuarantineS = envFloat("FH2MP_OWNER_QUARANTINE", 30)
	CarPolicy        = envStr("FH2MP_CAR_POLICY", "kick")
	PartPolicy       = strings.ToLower(strings.TrimSpace(envStr("FH2MP_PART_POLICY", "fix")))
	UnknownCarPolicy = envStr("FH2MP_UNKNOWN_CAR_POLICY", "standin")
	StandinCar       = int64(envInt("FH2MP_STANDIN_CAR", 1215))
	SpeedPolicy      = envStr("FH2MP_SPEED_POLICY", "kick")
	CarCapture       = os.Getenv("FH2MP_CAR_CAPTURE") != ""
	CheaterPoolHours = envFloat("FH2MP_CHEATER_POOL_HOURS", 24)
	EventLoadTimeout = envFloat("FH2MP_EVENT_LOAD_TIMEOUT", 30)
	LspURL           = envStr("FH2MP_LSP", "http://127.0.0.1:5080")
	TracePackets     = Verbosity.Enabled(diag.Trace, "packet")
)

// ValidatePolicies is the start-up check Python does at import time.
func ValidatePolicies() error {
	switch PartPolicy {
	case "fix", "kick", "fix+kick", "log", "off":
	default:
		return fmt.Errorf("invalid FH2MP_PART_POLICY")
	}
	return nil
}

// ClientLimit is a convenience for the 12-client formats.
const ClientLimit = capacity.ClientLimit

// Introduce points (FH2MP_INTRODUCE_AT).
var IntroducePoints = []string{"finalise", "console", "handshake"}

// EntryOnlyRouters wait for the entry (introduce replays them then).
var EntryOnlyRouters = map[int]bool{2: true}

// TagMode is one tag mode's name, time limit and players per It car.
type TagMode struct {
	Name    string
	Seconds float64
	PerIt   int
}

// TagModes by RaceModes.id.
var TagModes = map[int]*TagMode{
	RaceModeKing:      {"King", 240.0, 6},
	RaceModeInfection: {"Infection", 240.0, 0},
}

// TagAssignRepeats: seconds after the first send.
var TagAssignRepeats = []float64{0.0, 0.4, 1.2, 3.0}
