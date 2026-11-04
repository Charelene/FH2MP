// Package social is friends, profile settings and gamerpics: the data behind Xbox Live's social services as the client's XSAPI asks for them
// (= server/social.py). There is no Xbox Live to ask, so the LSP answers those calls from here: a mutual friend graph, per-player profile settings
// and gamerpics (a player without an uploaded one gets a generated identicon). Presence is a lease: a player is online while their client keeps
// polling the LSP.
package social

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"sync"
	"time"

	"fh2mp/internal/db"
	"fh2mp/internal/pngtool"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/ugc"
)

// Constants.
const (
	MaxGamerpic    = 1_000_000 // bytes of an uploaded PNG
	OnlineSeconds  = 40        // a client polls the Message Center every 5 s
	FH2TitleID     = "446059611"
	pngSignature   = "\x89PNG\r\n\x1a\n"
	timeFormat     = "2006-01-02T15:04:05.0000000Z"
	defaultTier    = "Gold"
	defaultPicSize = 160
)

// Now is the clock in seconds since the epoch (tests replace it).
var Now = func() float64 { return float64(time.Now().UnixNano()) / 1e9 }

var (
	pollMu   sync.Mutex
	lastPoll = map[int64]float64{} // xuid -> time of the last request, for presence (in memory on purpose: a lease)
)

// Touch records that a player's client just made a request.
func Touch(xuid int64) {
	if xuid != 0 {
		pollMu.Lock()
		lastPoll[xuid] = Now()
		pollMu.Unlock()
	}
}

// Online says whether a player's client polled within the lease.
func Online(xuid int64) bool {
	pollMu.Lock()
	t := lastPoll[xuid]
	pollMu.Unlock()
	return Now()-t < OnlineSeconds
}

// OnlineCount is the number of players whose lease is valid.
func OnlineCount() int {
	now := Now()
	pollMu.Lock()
	defer pollMu.Unlock()
	n := 0
	for _, t := range lastPoll {
		if now-t < OnlineSeconds {
			n++
		}
	}
	return n
}

// Polled is a copy of the lease table: xuid -> time of the last request.
func Polled() map[int64]float64 {
	pollMu.Lock()
	defer pollMu.Unlock()
	out := make(map[int64]float64, len(lastPoll))
	for x, t := range lastPoll {
		out[x] = t
	}
	return out
}

// ResetPresence forgets every lease (tests).
func ResetPresence() {
	pollMu.Lock()
	lastPoll = map[int64]float64{}
	pollMu.Unlock()
}

// Identicon is a deterministic 5x5 mirrored block picture (PNG) in the colour of the xuid's hash.
func Identicon(xuid int64, size int) []byte {
	h := sha256.Sum256([]byte(fmt.Sprintf("fh2mp-gamerpic-%d", xuid)))
	colour := [3]byte{byte(60 + int(h[0])%160), byte(60 + int(h[1])%160), byte(60 + int(h[2])%160)}
	var cells [5][5]bool
	for y := 0; y < 5; y++ {
		for x := 0; x < 5; x++ {
			cells[y][x] = h[3+y*3+min(x, 4-x)]&1 == 1
		}
	}
	bg, cell := [3]byte{240, 240, 240}, size/7
	off := (size - cell*5) / 2
	rows := make([][]byte, size)
	for py := 0; py < size; py++ {
		row := make([]byte, 0, size*3)
		for px := 0; px < size; px++ {
			cx, cy := floorDiv(px-off, cell), floorDiv(py-off, cell)
			on := cx >= 0 && cx < 5 && cy >= 0 && cy < 5 && px >= off && py >= off && cells[cy][cx]
			if on {
				row = append(row, colour[:]...)
			} else {
				row = append(row, bg[:]...)
			}
		}
		rows[py] = row
	}
	return pngtool.Encode(size, size, 3, rows)
}

func floorDiv(a, b int) int {
	q := a / b
	if a%b != 0 && (a < 0) != (b < 0) {
		q--
	}
	return q
}

// ValueError is a refused input (Python's ValueError).
type ValueError struct{ Msg string }

func (e *ValueError) Error() string { return e.Msg }

// Store is the friend graph and the profile data.
type Store struct {
	DB         *db.DB
	BotName    func(xuid int64) string // name of a shipped Drivatar bot (wins over any players row: a bot is never a player), set by the LSP
	NameLookup func(xuid int64) string // gamertag for other ids without a players row (stored player Drivatars), set by the LSP
}

// New makes a store.
func New(d *db.DB) *Store { return &Store{DB: d} }

// ---------------------------------------------------------------- friends

// Friends lists a player's friends, oldest friendship first.
func (s *Store) Friends(xuid int64) ([]int64, error) {
	rows, err := s.DB.Query("SELECT CASE WHEN a=? THEN b ELSE a END FROM friendships WHERE a=? OR b=? ORDER BY created", xuid, xuid, xuid)
	if err != nil {
		return nil, err
	}
	out := make([]int64, len(rows))
	for i := range rows {
		out[i] = rows[i].IntAt(0)
	}
	return out, nil
}

// AreFriends says whether two players are friends.
func (s *Store) AreFriends(x, y int64) (bool, error) {
	r, err := s.DB.One("SELECT 1 FROM friendships WHERE a=? AND b=?", min(x, y), max(x, y))
	return r != nil, err
}

// AddFriend makes two players friends (a repeat changes nothing).
func (s *Store) AddFriend(x, y int64) error {
	if x == y {
		return &ValueError{"a player cannot befriend themselves"}
	}
	return s.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("INSERT INTO friendships (a, b, created) VALUES (?,?,?) ON CONFLICT DO NOTHING", min(x, y), max(x, y), Now())
		return err
	})
}

// RemoveFriend ends a friendship; true when there was one.
func (s *Store) RemoveFriend(x, y int64) (bool, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		r, err := t.Exec("DELETE FROM friendships WHERE a=? AND b=?", min(x, y), max(x, y))
		n = r.RowsAffected
		return err
	})
	return n > 0, err
}

// ---------------------------------------------------------------- profiles

// Profile is everything XSAPI's profile settings can ask for (the wire values are strings).
type Profile struct {
	Gamertag, GameDisplayName, AppDisplayName, Gamerscore, AccountTier string
	PicVersion                                                         int64
}

// Get returns a profile setting by its XSAPI name ("" for one the server does not fill).
func (p *Profile) Get(setting string) string {
	switch setting {
	case "Gamertag":
		return p.Gamertag
	case "GameDisplayName":
		return p.GameDisplayName
	case "AppDisplayName":
		return p.AppDisplayName
	case "Gamerscore":
		return p.Gamerscore
	case "AccountTier":
		return p.AccountTier
	case "TenureLevel", "ShowUserAsAvatar":
		return "0"
	case "XboxOneRep":
		return "GoodPlayer"
	}
	return ""
}

// Profile reads a player's profile.
func (s *Store) Profile(xuid int64) (*Profile, error) {
	p, err := s.DB.One("SELECT display_name, gamerscore, tier, gamerpic_at FROM player_profiles WHERE xuid=?", xuid)
	if err != nil {
		return nil, err
	}
	pl, err := s.DB.One("SELECT gamertag FROM players WHERE xuid=?", xuid)
	if err != nil {
		return nil, err
	}
	tag := ""
	if s.BotName != nil {
		tag = s.BotName(xuid)
	}
	if tag == "" && pl != nil {
		tag = pl.Str("gamertag")
	}
	if tag == "" && s.NameLookup != nil {
		tag = s.NameLookup(xuid)
	}
	if tag == "" {
		tag = fmt.Sprintf("Player%d", ((xuid%100000)+100000)%100000)
	}
	out := &Profile{Gamertag: tag, GameDisplayName: tag, AppDisplayName: tag, Gamerscore: "0", AccountTier: defaultTier}
	if p != nil {
		if n := p.Str("display_name"); n != "" {
			out.GameDisplayName, out.AppDisplayName = n, n
		}
		out.Gamerscore = fmt.Sprint(p.Int("gamerscore"))
		out.AccountTier = p.Str("tier")
		out.PicVersion = int64(p.Float("gamerpic_at"))
	}
	return out, nil
}

// SetGamerpic stores an uploaded PNG.
func (s *Store) SetGamerpic(xuid int64, png []byte) error {
	if len(png) < 8 || !bytes.Equal(png[:8], []byte(pngSignature)) {
		return &ValueError{"the gamerpic must be a PNG"}
	}
	if len(png) > MaxGamerpic {
		return &ValueError{fmt.Sprintf("the gamerpic is larger than %d bytes", MaxGamerpic)}
	}
	return s.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("INSERT INTO player_profiles (xuid, gamerpic, gamerpic_at) VALUES (?,?,?) ON CONFLICT (xuid) DO UPDATE SET "+
			"gamerpic=excluded.gamerpic, gamerpic_at=excluded.gamerpic_at", xuid, ugc.Pack(png), Now())
		return err
	})
}

// ClearGamerpic goes back to the generated picture.
func (s *Store) ClearGamerpic(xuid int64) error {
	return s.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("UPDATE player_profiles SET gamerpic=NULL, gamerpic_at=? WHERE xuid=?", Now(), xuid)
		return err
	})
}

// SetProfile changes the given settings (nil = leave alone).
func (s *Store) SetProfile(xuid int64, displayName *string, gamerscore *int64, tier *string) error {
	return s.DB.Do(func(t *db.Tx) error {
		if _, err := t.Exec("INSERT INTO player_profiles (xuid) VALUES (?) ON CONFLICT DO NOTHING", xuid); err != nil {
			return err
		}
		if displayName != nil {
			if _, err := t.Exec("UPDATE player_profiles SET display_name=? WHERE xuid=?", *displayName, xuid); err != nil {
				return err
			}
		}
		if gamerscore != nil {
			if _, err := t.Exec("UPDATE player_profiles SET gamerscore=? WHERE xuid=?", *gamerscore, xuid); err != nil {
				return err
			}
		}
		if tier != nil {
			if _, err := t.Exec("UPDATE player_profiles SET tier=? WHERE xuid=?", *tier, xuid); err != nil {
				return err
			}
		}
		return nil
	})
}

// Gamerpic is the PNG bytes: the uploaded one, else a generated identicon.
func (s *Store) Gamerpic(xuid int64) ([]byte, error) {
	r, err := s.DB.One("SELECT gamerpic FROM player_profiles WHERE xuid=?", xuid)
	if err != nil {
		return nil, err
	}
	if r != nil && len(r.Bytes("gamerpic")) > 0 {
		return ugc.Unpack(r.Bytes("gamerpic"))
	}
	return Identicon(xuid, defaultPicSize), nil
}

// PlayerRow is one line of the player list.
type PlayerRow struct {
	Xuid        int64
	Gamertag    string
	CareerLevel int64
	Friends     int64
	HasGamerpic bool
}

// Players lists every named player, most recently seen first.
func (s *Store) Players() ([]PlayerRow, error) {
	rows, err := s.DB.Query("SELECT p.xuid, p.gamertag, p.career_level, (SELECT COUNT(*) FROM friendships f WHERE f.a=p.xuid OR f.b=p.xuid) AS friends, " +
		"(pp.gamerpic IS NOT NULL) AS has_gamerpic FROM players p LEFT JOIN player_profiles pp ON pp.xuid=p.xuid " +
		"WHERE p.gamertag <> '' ORDER BY p.last_seen DESC")
	if err != nil {
		return nil, err
	}
	out := make([]PlayerRow, len(rows))
	for i := range rows {
		r := &rows[i]
		out[i] = PlayerRow{r.Int("xuid"), r.Str("gamertag"), r.Int("career_level"), r.Int("friends"), r.Int("has_gamerpic") != 0}
	}
	return out, nil
}

// ---------------------------------------------------------------- the XSAPI answers (the LSP serialises them)

func obj(kv ...any) pyjson.Obj {
	o := make(pyjson.Obj, 0, len(kv)/2)
	for i := 0; i < len(kv); i += 2 {
		o = append(o, pyjson.Pair{K: kv[i].(string), V: kv[i+1]})
	}
	return o
}

// ProfileUser is one user of a profile settings answer; picURL makes the picture URL for (xuid, version).
func ProfileUser(s *Store, xuid int64, settings []string, picURL func(xuid, version int64) string) (pyjson.Obj, error) {
	prof, err := s.Profile(xuid)
	if err != nil {
		return nil, err
	}
	url := picURL(xuid, prof.PicVersion)
	vals := make([]any, len(settings))
	for i, name := range settings {
		var v any = prof.Get(name)
		switch name {
		case "GameDisplayPicRaw", "AppDisplayPicRaw":
			v = url
		case "_pic_version": // not a real setting, but Python's dict lookup answers it with the number
			v = prof.PicVersion
		}
		vals[i] = obj("id", name, "value", v)
	}
	return obj("id", fmt.Sprint(xuid), "hostId", fmt.Sprint(xuid), "settings", vals, "isSponsoredUser", false), nil
}

// People is the friend list answer.
func People(s *Store, xuid int64) (pyjson.Obj, error) {
	fr, err := s.Friends(xuid)
	if err != nil {
		return nil, err
	}
	list := make([]any, len(fr))
	for i, f := range fr {
		list[i] = obj("xuid", fmt.Sprint(f), "isFavorite", false, "isFollowingCaller", true, "socialNetworks", []any{})
	}
	return obj("people", list, "totalCount", len(fr)), nil
}

// Summary is the social summary answer.
func Summary(s *Store, xuid int64) (pyjson.Obj, error) {
	fr, err := s.Friends(xuid)
	if err != nil {
		return nil, err
	}
	n := len(fr)
	return obj("targetFollowingCount", n, "targetFollowerCount", n, "isCallerFollowingTarget", false, "isTargetFollowingCaller", false,
		"hasCallerMarkedTargetAsFavorite", false, "hasCallerMarkedTargetAsIdentityShared", false, "legacyFriendStatus", "None",
		"availablePeopleSlots", 1000, "recentChangeCount", 0, "watermark", "0"), nil
}

// Presence is the presence answer of one player.
func Presence(xuid int64) pyjson.Obj {
	if !Online(xuid) {
		return obj("xuid", fmt.Sprint(xuid), "state", "Offline")
	}
	t := time.Unix(int64(Now()), 0).UTC().Format(timeFormat)
	return obj("xuid", fmt.Sprint(xuid), "state", "Online", "devices", []any{obj("type", "XboxOne", "titles", []any{
		obj("id", FH2TitleID, "name", "Forza Horizon 2", "placement", "Full", "state", "Active", "lastModified", t)})})
}
