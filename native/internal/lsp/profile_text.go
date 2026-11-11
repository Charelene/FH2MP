package lsp

import (
	"fmt"
	"strings"
	"unicode"
	"unicode/utf8"

	"fh2mp/internal/auth"
	"fh2mp/internal/db"
	"fh2mp/internal/pyjson"
)

// A player's profile texts, the Xbox 360's four: motto, name, location, bio (/v1/community/profile). Anyone who can see the player's card
// sees them (they ride on the card), so they pass the operator's blocklist before they are stored.

// profileField is one text of the profile: its JSON key, its column and how long it may be (characters).
type profileField struct {
	key, column string
	max         int
}

var profileFields = []profileField{{"motto", "motto", 21}, {"name", "real_name", 40}, {"location", "location", 40}, {"bio", "bio", 500}}

// cleanProfileText is the text as stored: trimmed, inner runs of spaces as one; an error when it is too long, has control characters, or
// holds a blocked fragment or word. The refusal's words are the 360's.
func cleanProfileText(f profileField, value string, blocked, blockedWords []string) (string, error) {
	for _, r := range value {
		if unicode.IsControl(r) {
			return "", auth.NewError(400, "bad_profile_text", "Your "+f.key+" contains a character that isn't allowed. Please try again.").WithDetail(fmt.Sprintf("%s has the control character %U", f.key, r))
		}
	}
	text := strings.Join(strings.Fields(value), " ")
	if n := utf8.RuneCountInString(text); n > f.max {
		return "", auth.NewError(400, "bad_profile_text", fmt.Sprintf("Your %s can have up to %d characters.", f.key, f.max)).WithDetail(fmt.Sprintf("%s has %d characters", f.key, n))
	}
	folded := auth.FoldTag(text)
	for _, bad := range blocked {
		if bad != "" && strings.Contains(folded, bad) {
			return "", auth.NewError(400, "bad_profile_text", "Your "+f.key+" contains a word or phrase that isn't allowed. Please try again.").WithDetail(fmt.Sprintf("%s holds the blocked fragment %q", f.key, bad))
		}
	}
	if len(blockedWords) > 0 {
		words := map[string]bool{}
		for _, w := range strings.FieldsFunc(text, func(r rune) bool { return !unicode.IsLetter(r) && !unicode.IsDigit(r) }) {
			words[auth.FoldTag(w)] = true
		}
		for _, bad := range blockedWords {
			if words[bad] {
				return "", auth.NewError(400, "bad_profile_text", "Your "+f.key+" contains a word or phrase that isn't allowed. Please try again.").WithDetail(fmt.Sprintf("%s holds the blocked word %q", f.key, bad))
			}
		}
	}
	return text, nil
}

// ownProfile is GET /v1/community/profile: the player's own four texts.
func (l *Launcher) ownProfile(me int64) (any, error) {
	row, err := l.C.DB.One("SELECT motto, real_name, location, bio FROM player_profiles WHERE xuid=?", me)
	if err != nil {
		return nil, err
	}
	out := pyjson.Obj{}
	for _, f := range profileFields {
		v := ""
		if row != nil {
			v = row.Str(f.column)
		}
		out = append(out, pyjson.Pair{K: f.key, V: v})
	}
	return out, nil
}

// setProfile is POST /v1/community/profile {"motto"?, "name"?, "location"?, "bio"?}: the texts named are replaced (an empty one clears
// it), the others stay. Nothing is stored when one of them is refused.
func (l *Launcher) setProfile(me int64, body pyjson.Obj) (any, error) {
	type change struct {
		f    profileField
		text string
	}
	var changes []change
	for _, f := range profileFields {
		v, has := body.Get(f.key)
		if !has {
			continue
		}
		s, isStr := v.(string)
		if !isStr {
			return nil, auth.NewError(400, "bad_request", "").WithXuid(me).WithDetail(f.key + " is " + pyjson.TypeName(v))
		}
		text, err := cleanProfileText(f, s, l.C.Auth.Blocked, l.C.Auth.BlockedWords)
		if err != nil {
			if ae, ok := err.(*auth.Error); ok {
				return nil, ae.WithXuid(me)
			}
			return nil, err
		}
		changes = append(changes, change{f, text})
	}
	if len(changes) == 0 {
		return nil, auth.NewError(400, "bad_request", "").WithXuid(me).WithDetail("no profile text named")
	}
	err := l.C.DB.Do(func(t *db.Tx) error {
		if _, err := t.Exec("INSERT INTO player_profiles (xuid) VALUES (?) ON CONFLICT DO NOTHING", me); err != nil {
			return err
		}
		for _, c := range changes {
			if _, err := t.Exec("UPDATE player_profiles SET "+c.f.column+"=? WHERE xuid=?", c.text, me); err != nil {
				return err
			}
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	names := make([]string, len(changes))
	for i, c := range changes {
		names[i] = fmt.Sprintf("%s (%d characters)", c.f.key, utf8.RuneCountInString(c.text))
	}
	communityLog.Info(fmt.Sprintf("profile texts set: xuid=%#x %s", me, strings.Join(names, ", ")))
	return l.ownProfile(me)
}
