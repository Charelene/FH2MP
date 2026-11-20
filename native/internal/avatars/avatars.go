// Package avatars is the players' Xbox 360 avatars as Encore's editor saves them (the 1000-byte manifest, the editor's colour choices and
// the full-body picture Encore renders) and the avatar items each account owns. Items are rewards, later The Eliminator's battle pass: there
// is no store and no money. An item in the reward catalog may only be worn by an account that owns it; every other item is free to wear.
// The launcher API (internal/lsp, /v1/me/avatar) checks what is sent; this package stores it.
package avatars

import (
	"encoding/hex"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"

	"fh2mp/internal/db"
	"fh2mp/internal/diag"
)

var log = diag.Get("avatars")

// ManifestSize is the size of an avatar manifest (XAVATAR_METADATA); ColoursMax the longest colour list kept, in bytes.
const (
	ManifestSize = 1000
	ColoursMax   = 16384
)

// The id slots of a manifest, as Encore reads them (crates/encore-avatar/src/manifest.rs and avedit/edit.rs, from 2.0.17559's files):
//
//	0x00C  15 x 16  face parts: chin, nose and ears, then six 32-byte face textures (mouth, eyes, eyebrows, facial hair, eye shadow,
//	                facial features). Read as fifteen 16-byte slots, as manifest.rs prints them, so a texture's second half is looked at too.
//	0x120  22 x 32  asset entries, each starting with its 16-byte id: body, head, 13 worn items, then 7 of the basic outfit, up to 0x3E0.
//	                The console puts a basic outfit piece back on when nothing worn covers its component (0x8196d0b8), so those count.
//
// An id of 16 zero bytes is an empty slot. The other bytes (the head at 0x000, the nine colours at 0x0FC, the 16 bytes after each entry's
// id, the 8 bytes at 0x3E0) hold no item ids.
const (
	FaceAt, FaceSlots, FaceStep    = 0x0C, 15, 16
	AssetAt, AssetSlots, AssetStep = 0x120, 22, 32
)

// ItemID is a 16-byte avatar asset id in the byte order the manifest holds it. Its text form is 32 lowercase hex digits.
type ItemID [16]byte

// String is the text form: 32 lowercase hex digits.
func (id ItemID) String() string { return hex.EncodeToString(id[:]) }

// IsZero reports the empty slot.
func (id ItemID) IsZero() bool { return id == ItemID{} }

// ParseItem reads an item id: 32 hex digits, either case. The id of 16 zero bytes is an empty slot, not an item.
func ParseItem(s string) (ItemID, error) {
	var id ItemID
	if len(s) != 32 {
		return id, fmt.Errorf("an item id is 32 hex digits, this has %d characters", len(s))
	}
	if _, err := hex.Decode(id[:], []byte(s)); err != nil {
		return id, fmt.Errorf("an item id is 32 hex digits: %v", err)
	}
	if id.IsZero() {
		return id, errors.New("16 zero bytes are an empty slot, not an item")
	}
	return id, nil
}

// Worn lists the item ids a manifest of ManifestSize bytes names, face slots first, then asset entries, each id once.
func Worn(m []byte) []ItemID {
	if len(m) != ManifestSize {
		return nil
	}
	var out []ItemID
	seen := map[ItemID]bool{}
	add := func(o int) {
		var id ItemID
		copy(id[:], m[o:o+16])
		if !id.IsZero() && !seen[id] {
			seen[id] = true
			out = append(out, id)
		}
	}
	for s := 0; s < FaceSlots; s++ {
		add(FaceAt + FaceStep*s)
	}
	for s := 0; s < AssetSlots; s++ {
		add(AssetAt + AssetStep*s)
	}
	return out
}

// Rewards is the reward catalog: the items an account must own to wear them (The Eliminator's battle-pass rewards, picked when the battle
// pass is designed). Empty for now, so every item a manifest can name is free to wear.
var Rewards = []ItemID{}

var colourLine = regexp.MustCompile(`^[0-9]{1,6} [0-9]{1,6}$`)

// CheckColours: the editor's colour list (avedit's .colours file) is lines of "<record> <palette>", each ended by a newline, at most
// ColoursMax bytes; "" is no choices.
func CheckColours(s string) error {
	if len(s) > ColoursMax {
		return fmt.Errorf("%d bytes, the limit is %d", len(s), ColoursMax)
	}
	if s == "" {
		return nil
	}
	if !strings.HasSuffix(s, "\n") {
		return errors.New("the last line does not end with a newline")
	}
	for i, l := range strings.Split(strings.TrimSuffix(s, "\n"), "\n") {
		if !colourLine.MatchString(l) {
			return fmt.Errorf("line %d is not two numbers of 1 to 6 digits with one space between them", i+1)
		}
	}
	return nil
}

var sourceRe = regexp.MustCompile(`^[A-Za-z0-9._:-]{1,64}$`)

// RefusedError is a grant the store does not make: no account has the xuid, the item is the empty id, or the source is not one.
type RefusedError struct{ Reason string }

func (e *RefusedError) Error() string { return "avatars: grant refused: " + e.Reason }

// NotOwnedError refuses a save: the manifest wears reward items the account does not own.
type NotOwnedError struct{ Items []ItemID }

func (e *NotOwnedError) Error() string {
	ids := make([]string, len(e.Items))
	for i, id := range e.Items {
		ids[i] = id.String()
	}
	return "reward items not owned: " + strings.Join(ids, ", ")
}

// Store keeps the avatars and the owned items.
type Store struct {
	DB      *db.DB
	Rewards map[ItemID]bool // the reward catalog (Rewards; tests put their own items here)
	Now     func() float64  // seconds since the epoch: versions and grant times (tests replace it)
}

// New makes the store with the reward catalog of this build.
func New(d *db.DB) *Store {
	r := map[ItemID]bool{}
	for _, id := range Rewards {
		r[id] = true
	}
	return &Store{DB: d, Rewards: r, Now: func() float64 { return float64(time.Now().UnixNano()) / 1e9 }}
}

// Avatar is a stored avatar without its picture. Version is the time of its last save, raised by every save (the card's avatarVersion).
type Avatar struct {
	Manifest []byte
	Colours  string
	Version  float64
}

// Owned is an item an account owns.
type Owned struct {
	Item      ItemID
	GrantedAt float64
	Source    string
}

// Save stores an avatar (manifest of ManifestSize bytes, colours and the body PNG as checked by the caller) and returns its new version. It
// refuses with *NotOwnedError when the manifest wears reward items the account does not own; the check and the write are one transaction.
// A version is the save's time, and always above the version it replaces (by a millisecond if the clock has not moved on).
func (s *Store) Save(xuid int64, manifest []byte, colours string, body []byte) (float64, error) {
	if len(manifest) != ManifestSize {
		return 0, fmt.Errorf("avatars: a manifest is %d bytes, this is %d", ManifestSize, len(manifest))
	}
	var rewards []ItemID
	for _, id := range Worn(manifest) {
		if s.Rewards[id] {
			rewards = append(rewards, id)
		}
	}
	var version float64
	err := s.DB.Do(func(t *db.Tx) error {
		if len(rewards) > 0 {
			args := []any{xuid}
			for _, id := range rewards {
				args = append(args, id.String())
			}
			rows, err := t.Query("SELECT item FROM avatar_items WHERE xuid=? AND item IN ("+strings.TrimSuffix(strings.Repeat("?,", len(rewards)), ",")+")", args...)
			if err != nil {
				return err
			}
			have := map[string]bool{}
			for _, r := range rows {
				have[r.StrAt(0)] = true
			}
			var missing []ItemID
			for _, id := range rewards {
				if !have[id.String()] {
					missing = append(missing, id)
				}
			}
			if len(missing) > 0 {
				return &NotOwnedError{missing}
			}
		}
		rows, err := t.Query("INSERT INTO avatars (xuid, manifest, colours, body_png, updated_at) VALUES (?,?,?,?,?) ON CONFLICT (xuid) DO UPDATE SET "+
			"manifest=excluded.manifest, colours=excluded.colours, body_png=excluded.body_png, updated_at=GREATEST(excluded.updated_at, avatars.updated_at + 0.001) "+
			"RETURNING updated_at", xuid, manifest, colours, body, s.Now())
		if err != nil {
			return err
		}
		if len(rows) != 1 {
			return fmt.Errorf("avatars: the save of %#x returned %d rows", xuid, len(rows))
		}
		version = rows[0].FloatAt(0)
		return nil
	})
	return version, err
}

// Get is an account's avatar (nil = none).
func (s *Store) Get(xuid int64) (*Avatar, error) {
	r, err := s.DB.One("SELECT manifest, colours, updated_at FROM avatars WHERE xuid=?", xuid)
	if err != nil || r == nil {
		return nil, err
	}
	return &Avatar{Manifest: r.Bytes("manifest"), Colours: r.Str("colours"), Version: r.Float("updated_at")}, nil
}

// Body is an account's full-body picture (nil = no avatar).
func (s *Store) Body(xuid int64) ([]byte, error) {
	r, err := s.DB.One("SELECT body_png FROM avatars WHERE xuid=?", xuid)
	if err != nil || r == nil {
		return nil, err
	}
	return r.Bytes("body_png"), nil
}

// Delete removes an account's avatar (false = there was none). The owned items stay.
func (s *Store) Delete(xuid int64) (bool, error) {
	var gone bool
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("DELETE FROM avatars WHERE xuid=?", xuid)
		gone = res.RowsAffected > 0
		return err
	})
	return gone, err
}

// Items are the items an account owns, the oldest grant first.
func (s *Store) Items(xuid int64) ([]Owned, error) {
	rows, err := s.DB.Query("SELECT item, granted_at, source FROM avatar_items WHERE xuid=? ORDER BY granted_at, item", xuid)
	if err != nil {
		return nil, err
	}
	out := make([]Owned, 0, len(rows))
	for _, r := range rows {
		id, err := ParseItem(r.StrAt(0))
		if err != nil {
			return nil, fmt.Errorf("avatars: stored item %q of %#x: %w", r.StrAt(0), xuid, err)
		}
		out = append(out, Owned{Item: id, GrantedAt: r.FloatAt(1), Source: r.StrAt(2)})
	}
	return out, nil
}

// Grant gives an account an item; source says what gave it ("battlepass:<season>:<tier>", "test"; 1 to 64 of A-Z a-z 0-9 . _ : -). It is
// the one way items are owned (the battle pass calls it; the operators' /admin/avatar-items line for tests). granted=false: the account
// already owned it, and its first grant stays as it was. Any item id can be granted; only those in the catalog need owning to be worn.
// A *RefusedError names what was wrong with the grant; other errors are the database's.
func (s *Store) Grant(xuid int64, item ItemID, source string) (granted bool, err error) {
	if item.IsZero() {
		return false, &RefusedError{"16 zero bytes are an empty slot, not an item"}
	}
	if !sourceRe.MatchString(source) {
		return false, &RefusedError{fmt.Sprintf("the source %q is not 1 to 64 of A-Z a-z 0-9 . _ : -", source)}
	}
	now := s.Now()
	err = s.DB.Do(func(t *db.Tx) error {
		acct, err := t.One("SELECT 1 FROM accounts WHERE xuid=?", xuid)
		if err != nil {
			return err
		}
		if acct == nil {
			return &RefusedError{fmt.Sprintf("no account has the xuid %#x", xuid)}
		}
		res, err := t.Exec("INSERT INTO avatar_items (xuid, item, granted_at, source) VALUES (?,?,?,?) ON CONFLICT (xuid, item) DO NOTHING", xuid, item.String(), now, source)
		granted = res.RowsAffected > 0
		return err
	})
	if err != nil {
		return false, err
	}
	if granted {
		reward := "free to wear without it"
		if s.Rewards[item] {
			reward = "a reward item"
		}
		log.Info(fmt.Sprintf("item %s granted to %#x by %s (%s)", item, xuid, source, reward))
	}
	return granted, nil
}
