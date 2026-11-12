package lsp

import (
	"errors"
	"strings"
	"testing"

	"fh2mp/internal/auth"
)

func bookAt(t *float64) *inviteBook {
	b := newInviteBook()
	b.now = func() float64 { return *t }
	return b
}

func errCode(err error) string {
	var e *auth.Error
	if errors.As(err, &e) {
		return e.Code
	}
	return ""
}

func TestInviteReachesOnlyItsRecipientNewestFirst(t *testing.T) {
	now := 1000.0
	b := bookAt(&now)
	if _, err := b.Add(1, 2, "first", "lobbyA", "freeroam"); err != nil {
		t.Fatal(err)
	}
	now += 20
	if _, err := b.Add(3, 2, "second", "lobbyB", "freeroam"); err != nil {
		t.Fatal(err)
	}
	got := b.List(2)
	if len(got) != 2 || got[0].Text != "second" || got[1].Text != "first" {
		t.Fatalf("recipient 2 sees %+v", got)
	}
	if len(b.List(1)) != 0 {
		t.Fatal("the sender must not see its own invite")
	}
}

func TestInviteExpiresAfterAnHour(t *testing.T) {
	now := 1000.0
	b := bookAt(&now)
	b.Add(1, 2, "hi", "L", "freeroam")
	now += inviteKeep - 1
	if len(b.List(2)) != 1 {
		t.Fatal("invite gone early")
	}
	now += 2
	if len(b.List(2)) != 0 {
		t.Fatal("invite outlived its keep time")
	}
}

func TestInviteSpamIsRefused(t *testing.T) {
	now := 1000.0
	b := bookAt(&now)
	b.Add(1, 2, "a", "L", "freeroam")
	now += 3
	if _, err := b.Add(1, 2, "b", "L", "freeroam"); errCode(err) != "invite_too_fast" {
		t.Fatalf("second invite 3 s later: %v", err)
	}
	now += inviteGap
	if _, err := b.Add(1, 2, "c", "L", "freeroam"); err != nil {
		t.Fatalf("after the gap: %v", err)
	}
	if got := b.List(2); len(got) != 1 || got[0].Text != "c" {
		t.Fatalf("a repeat invite for the same lobby must replace the old one: %+v", got)
	}
}

func TestInviteTakeIsOnlyForTheRecipient(t *testing.T) {
	now := 1000.0
	b := bookAt(&now)
	iv, _ := b.Add(1, 2, "hi", "L", "freeroam")
	if _, ok := b.Take(3, iv.ID); ok {
		t.Fatal("another player took someone else's invite")
	}
	got, ok := b.Take(2, iv.ID)
	if !ok || got.Lobby != "L" {
		t.Fatalf("recipient could not take it: %+v %v", got, ok)
	}
	if _, ok := b.Take(2, iv.ID); ok {
		t.Fatal("an invite was taken twice")
	}
}

func TestInviteTextRules(t *testing.T) {
	now := 1000.0
	b := bookAt(&now)
	if iv, err := b.Add(1, 2, "  has invited you to play  ", "L", "freeroam"); err != nil || iv.Text != "has invited you to play" {
		t.Fatalf("text not trimmed: %+v %v", iv, err)
	}
	now += 20
	if _, err := b.Add(1, 2, strings.Repeat("x", inviteTextRunes+1), "M", "freeroam"); errCode(err) != "bad_message" {
		t.Fatalf("overlong text: %v", err)
	}
	if _, err := b.Add(1, 2, "bad\x07", "M", "freeroam"); errCode(err) != "bad_message" {
		t.Fatalf("control character: %v", err)
	}
	if iv, err := b.Add(1, 2, "", "M", "freeroam"); err != nil || iv.Text != "" {
		t.Fatalf("an invite needs no text: %v", err)
	}
}

func TestInviteLimitsPerRecipientAndSender(t *testing.T) {
	now := 1000.0
	b := bookAt(&now)
	for s := int64(10); s < 10+invitePending; s++ {
		if _, err := b.Add(s, 2, "", "L", "freeroam"); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := b.Add(99, 2, "", "L", "freeroam"); errCode(err) != "invites_full" {
		t.Fatalf("recipient with %d pending: %v", invitePending, err)
	}
	for to := int64(100); to < 100+inviteSenderCap; to++ {
		if _, err := b.Add(5, to, "", "L", "freeroam"); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := b.Add(5, 500, "", "L", "freeroam"); errCode(err) != "invite_limit" {
		t.Fatalf("sender over its cap: %v", err)
	}
}
