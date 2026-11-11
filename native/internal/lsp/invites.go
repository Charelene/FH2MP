package lsp

import (
	"fmt"
	"sort"
	"sync"
	"unicode/utf8"

	"fh2mp/internal/auth"
)

// Game invites, the way the Xbox 360 Guide had them: one friend at a time, online or not (an offline friend finds it in Messages when they
// sign in, like the 360's inbox), sent from inside a game, with an optional short text. An invite lives in memory for an hour: it names the lobby the sender was in when it was sent, and accepting it takes a seat there
// (the same lease the launcher's Join button makes). The Guide and the launcher read them from /v1/community/invites.

const (
	inviteKeep      = 3600.0 // seconds an invite stays pending (accepting fails once its lobby is gone)
	inviteGap       = 10.0  // seconds between two invites from one player to the same friend
	invitePending   = 10    // pending invites per recipient
	inviteTextRunes = 256
	inviteSenderCap = 30 // invites one player may send per inviteKeep window
)

type invite struct {
	ID      int64
	From    int64
	To      int64
	Text    string
	Lobby   string
	Kind    string
	Created float64
}

type inviteBook struct {
	mu       sync.Mutex
	next     int64
	byTo     map[int64][]*invite
	lastSent map[[2]int64]float64
	sent     map[int64][]float64
	now      func() float64
}

func newInviteBook() *inviteBook {
	return &inviteBook{byTo: map[int64][]*invite{}, lastSent: map[[2]int64]float64{}, sent: map[int64][]float64{}, now: nowF}
}

// cleanInviteText trims the text and refuses control characters and anything over inviteTextRunes.
func cleanInviteText(s string) (string, error) {
	s = pyStrip(s)
	if utf8.RuneCountInString(s) > inviteTextRunes {
		return "", auth.NewError(400, "bad_message", fmt.Sprintf("An invite message can hold up to %d characters.", inviteTextRunes))
	}
	for _, ch := range s {
		if ch < 32 || ch == 0x7f {
			return "", auth.NewError(400, "bad_message", "An invite message cannot contain control characters.")
		}
	}
	return s, nil
}

func (b *inviteBook) pruneLocked(now float64) {
	for to, list := range b.byTo {
		keep := list[:0]
		for _, iv := range list {
			if now-iv.Created < inviteKeep {
				keep = append(keep, iv)
			}
		}
		if len(keep) == 0 {
			delete(b.byTo, to)
		} else {
			b.byTo[to] = keep
		}
	}
	for k, t := range b.lastSent {
		if now-t >= inviteGap {
			delete(b.lastSent, k)
		}
	}
}

// Add records an invite. A second invite from the same sender to the same friend for the same lobby replaces the first instead of stacking.
func (b *inviteBook) Add(from, to int64, text, lobby, kind string) (*invite, error) {
	text, err := cleanInviteText(text)
	if err != nil {
		return nil, err
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	now := b.now()
	b.pruneLocked(now)
	if now-b.lastSent[[2]int64{from, to}] < inviteGap && b.lastSent[[2]int64{from, to}] != 0 {
		return nil, auth.NewError(429, "invite_too_fast", "You just invited that player. Give them a moment.").WithXuid(from)
	}
	recent := b.sent[from][:0]
	for _, t := range b.sent[from] {
		if now-t < inviteKeep {
			recent = append(recent, t)
		}
	}
	if len(recent) >= inviteSenderCap {
		b.sent[from] = recent
		return nil, auth.NewError(429, "invite_limit", "You have sent a lot of invites. Try again in a few minutes.").WithXuid(from)
	}
	list := b.byTo[to]
	for i, iv := range list {
		if iv.From == from && iv.Lobby == lobby {
			list = append(list[:i], list[i+1:]...)
			break
		}
	}
	if len(list) >= invitePending {
		return nil, auth.NewError(409, "invites_full", "That player has too many pending invites.").WithXuid(from)
	}
	b.next++
	iv := &invite{ID: b.next, From: from, To: to, Text: text, Lobby: lobby, Kind: kind, Created: now}
	b.byTo[to] = append(list, iv)
	b.lastSent[[2]int64{from, to}] = now
	b.sent[from] = append(recent, now)
	return iv, nil
}

// List returns the recipient's pending invites, newest first.
func (b *inviteBook) List(to int64) []invite {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.pruneLocked(b.now())
	out := make([]invite, 0, len(b.byTo[to]))
	for _, iv := range b.byTo[to] {
		out = append(out, *iv)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID > out[j].ID })
	return out
}

// Take removes and returns one pending invite of the recipient (accepting or declining it).
func (b *inviteBook) Take(to, id int64) (*invite, bool) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.pruneLocked(b.now())
	list := b.byTo[to]
	for i, iv := range list {
		if iv.ID == id {
			b.byTo[to] = append(list[:i], list[i+1:]...)
			return iv, true
		}
	}
	return nil, false
}

// Get returns a pending invite of the recipient without removing it.
func (b *inviteBook) Get(to, id int64) (*invite, bool) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.pruneLocked(b.now())
	for _, iv := range b.byTo[to] {
		if iv.ID == id {
			c := *iv
			return &c, true
		}
	}
	return nil, false
}
