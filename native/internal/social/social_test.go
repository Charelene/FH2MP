package social

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"reflect"
	"strconv"
	"testing"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
	"fh2mp/internal/pngtool"
	"fh2mp/internal/pyjson"
)

type fixture struct {
	Identicon []struct {
		Xuid int64 `json:"xuid"`
		Size int   `json:"size"`
		Px   struct {
			W, H, Ch int
			Sha      string
		} `json:"px"`
	} `json:"identicon"`
	Steps []struct {
		S          string          `json:"s"`
		R          json.RawMessage `json:"r"`
		ValueError *string         `json:"value_error"`
	} `json:"steps"`
}

func load(t *testing.T) *fixture {
	raw, err := os.ReadFile("../../testdata/social.json")
	if err != nil {
		t.Skip(err)
	}
	var f fixture
	if err := json.Unmarshal(raw, &f); err != nil {
		t.Fatal(err)
	}
	return &f
}

func pixelSha(t *testing.T, png []byte) map[string]any {
	im, err := pngtool.Decode(png, pngtool.MaxDim)
	if err != nil {
		t.Fatal(err)
	}
	h := sha256.New()
	for _, r := range im.Rows {
		h.Write(r)
	}
	return map[string]any{"w": im.W, "h": im.H, "ch": im.Ch, "sha": hex.EncodeToString(h.Sum(nil))}
}

func TestIdenticon(t *testing.T) {
	f := load(t)
	for _, c := range f.Identicon {
		got := pixelSha(t, Identicon(c.Xuid, c.Size))
		if got["w"] != c.Px.W || got["h"] != c.Px.H || got["ch"] != c.Px.Ch || got["sha"] != c.Px.Sha {
			t.Errorf("identicon %d at %d differs: %v want %+v", c.Xuid, c.Size, got, c.Px)
		}
	}
}

func profMap(p *Profile) map[string]any {
	return map[string]any{"Gamertag": p.Gamertag, "GameDisplayName": p.GameDisplayName, "AppDisplayName": p.AppDisplayName, "Gamerscore": p.Gamerscore,
		"AccountTier": p.AccountTier, "RealName": "", "Bio": "", "Location": "", "TenureLevel": p.Get("TenureLevel"), "XboxOneRep": p.Get("XboxOneRep"),
		"PreferredColor": "", "WatermarkLabels": "", "ShowUserAsAvatar": p.Get("ShowUserAsAvatar"), "_pic_version": p.PicVersion}
}

func TestMatchesPython(t *testing.T) {
	f := load(t)
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	clock := 1000.0
	oldNow := Now
	Now = func() float64 { return clock }
	defer func() { Now = oldNow }()
	ResetPresence()
	for _, p := range []struct {
		x    int64
		tag  string
		seen float64
	}{{1, "Alice", 10}, {2, "Bob", 30}, {3, "carol", 20}, {4, "", 40}, {5, "Eve", 5}} {
		if _, err := d.Exec("INSERT INTO players (xuid, gamertag, career_level, first_seen, last_seen) VALUES (?,?,?,?,?)", p.x, p.tag, p.x*3, 1.0, p.seen); err != nil {
			t.Fatal(err)
		}
	}
	s := New(d)
	n := 0
	check := func(name string, got any, err error) {
		t.Helper()
		if n >= len(f.Steps) {
			t.Fatalf("%s: no more steps", name)
		}
		want := f.Steps[n]
		n++
		if want.S != name {
			t.Fatalf("step %d is %s in Python and %s in Go", n, want.S, name)
		}
		var ve *ValueError
		if errors.As(err, &ve) {
			if want.ValueError == nil || *want.ValueError != ve.Msg {
				t.Fatalf("%s: error %q, python %v", name, ve.Msg, want.ValueError)
			}
			return
		}
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if want.ValueError != nil {
			t.Fatalf("%s: python raised %q, Go did not", name, *want.ValueError)
		}
		if o, ok := got.(pyjson.Obj); ok {
			text, e := pyjson.Dumps(o, pyjson.Opts{})
			if e != nil {
				t.Fatal(e)
			}
			got = json.RawMessage(text)
		}
		b, _ := json.Marshal(got)
		var g, w any
		json.Unmarshal(b, &g)
		json.Unmarshal(want.R, &w)
		if !reflect.DeepEqual(g, w) {
			t.Fatalf("step %s differs\n got %s\nwant %s", name, b, want.R)
		}
	}
	for i, p := range [][2]int64{{1, 2}, {3, 2}, {3, 1}, {1, 3}, {2, 1}, {5, 1}} {
		clock = float64(1000 + i)
		check("add_"+itoa(p[0])+"_"+itoa(p[1]), nil, s.AddFriend(p[0], p[1]))
	}
	check("add_self", nil, s.AddFriend(4, 4))
	for _, x := range []int64{1, 2, 3, 4, 9} {
		fr, err := s.Friends(x)
		if fr == nil {
			fr = []int64{}
		}
		check("friends_"+itoa(x), fr, err)
	}
	ok, err := s.AreFriends(1, 2)
	check("are_1_2", ok, err)
	ok, err = s.AreFriends(2, 1)
	check("are_2_1", ok, err)
	ok, err = s.AreFriends(1, 4)
	check("are_1_4", ok, err)
	ok, err = s.RemoveFriend(1, 5)
	check("remove_5_1", ok, err)
	ok, err = s.RemoveFriend(1, 5)
	check("remove_5_1_again", ok, err)
	fr, err := s.Friends(1)
	check("friends_1_after", fr, err)

	prof := func(name string, x int64) {
		p, err := s.Profile(x)
		var m any
		if err == nil {
			m = profMap(p)
		}
		check(name, m, err)
	}
	for _, x := range []int64{1, 4, 9, 123456, -5} {
		prof("profile_"+itoa(x), x)
	}
	s.BotName = func(x int64) string {
		if x == 2 {
			return "BotBob"
		}
		return ""
	}
	s.NameLookup = func(x int64) string {
		if x == 9 {
			return "Stored9"
		}
		return ""
	}
	for _, x := range []int64{2, 9, 10} {
		prof("profile_hooks_"+itoa(x), x)
	}
	clock = 2000
	name, score, tier := "Alice D", int64(1234), "Silver"
	check("set_profile", nil, s.SetProfile(1, &name, &score, &tier))
	prof("profile_1_set", 1)
	seven := int64(7)
	check("set_profile_partial", nil, s.SetProfile(3, nil, &seven, nil))
	prof("profile_3_set", 3)
	empty := ""
	check("set_profile_empty_name", nil, s.SetProfile(1, &empty, nil, nil))
	prof("profile_1_name", 1)

	pic, err := s.Gamerpic(1)
	check("pic_default", map[string]any{"png": pixelSha(t, pic)}, err)
	png := pngtool.Encode(3, 2, 3, [][]byte{{0, 1, 2, 3, 4, 5, 6, 7, 8}, {9, 10, 11, 12, 13, 14, 15, 16, 17}})
	clock = 3000
	check("set_pic", nil, s.SetGamerpic(1, png))
	pic, err = s.Gamerpic(1)
	same := false
	if err == nil {
		// the Python script compares with its own encoding of the same pixels; here the upload is returned unchanged, which is the same property
		same = bytes.Equal(pic, png)
	}
	check("pic_uploaded", same, err)
	p, _ := s.Profile(1)
	check("profile_pic_version", p.PicVersion, nil)
	check("bad_pic", nil, s.SetGamerpic(1, []byte("GIF89a.....")))
	check("empty_pic", nil, s.SetGamerpic(1, nil))
	check("big_pic", nil, s.SetGamerpic(1, append([]byte(pngSignature), make([]byte, MaxGamerpic)...)))
	clock = 3500
	check("clear_pic", nil, s.ClearGamerpic(1))
	pic, err = s.Gamerpic(1)
	check("pic_after_clear", map[string]any{"png": pixelSha(t, pic)}, err)
	p, _ = s.Profile(1)
	check("profile_pic_version_cleared", p.PicVersion, nil)
	check("clear_unknown", nil, s.ClearGamerpic(99))
	clock = 3600
	check("set_pic_3", nil, s.SetGamerpic(3, png))
	pl, err := s.Players()
	rows := []any{}
	for _, r := range pl {
		rows = append(rows, map[string]any{"xuid": r.Xuid, "gamertag": r.Gamertag, "career_level": r.CareerLevel, "friends": r.Friends, "has_gamerpic": r.HasGamerpic})
	}
	check("players", rows, err)

	picURL := func(x, v int64) string { return "https://example.test/pic/" + itoa(x) + "?v=" + itoa(v) }
	pu, err := ProfileUser(s, 1, []string{"Gamertag", "GameDisplayName", "GameDisplayPicRaw", "Gamerscore", "Nope", "TenureLevel", "XboxOneRep", "AccountTier",
		"AppDisplayPicRaw", "RealName", "_pic_version"}, picURL)
	check("profile_user", pu, err)
	pu, err = ProfileUser(s, 9, []string{}, picURL)
	check("profile_user_none", pu, err)
	pe, err := People(s, 1)
	check("people", pe, err)
	pe, err = People(s, 4)
	check("people_none", pe, err)
	su, err := Summary(s, 3)
	check("summary", su, err)

	clock = 5000
	check("presence_offline", Presence(1), nil)
	Touch(1)
	Touch(0)
	clock = 5010
	check("online_10", Online(1), nil)
	check("presence_online", Presence(1), nil)
	Touch(2)
	clock = 5039.9
	check("online_39", []bool{Online(1), Online(2)}, nil)
	clock = 5040
	check("online_40", []bool{Online(1), Online(2)}, nil)
	clock = 5049.9
	check("online_count", OnlineCount(), nil)
	if n != len(f.Steps) {
		t.Fatalf("Go ran %d steps, Python %d", n, len(f.Steps))
	}
}

func itoa(v int64) string { return strconv.FormatInt(v, 10) }
