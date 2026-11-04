package lsp

import (
	"fmt"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"strconv"
	"strings"

	"fh2mp/internal/db"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/social"
)

// Xbox Live social services (profile, social, peoplehub, presence) as the client's XSAPI asks for them, and the admin / launcher API for friends and
// gamerpics.

func picURL(xuid, version int64) string {
	host := os.Getenv("FH2MP_PUBLIC_HOST")
	if host == "" {
		host = "127.0.0.1"
	}
	port := os.Getenv("FH2MP_LSP_PUBLIC_PORT")
	if port == "" {
		port = "5080"
	}
	return fmt.Sprintf("http://%s:%s/gamerpic/%d.png?v=%d", host, port, xuid, version)
}

var xuidSegRe = regexp.MustCompile(`^xuid\((\d+)\)$`)

// xuidOf is `xuid(123)` or `me` in an XSAPI path.
func (c *Core) xuidOf(r *Req, segment string, h http.Header) int64 {
	if m := xuidSegRe.FindStringSubmatch(segment); m != nil {
		n, _ := strconv.ParseInt(m[1], 10, 64)
		return n
	}
	n, _ := strconv.ParseInt(c.xblXuid(r, h), 10, 64)
	return n
}

var xuidTokenRe = regexp.MustCompile(`x=fh2mp;(\d+)`)

// xblXuid is the caller's Xbox user id as text ("" = none): the shim's XSAPI token is "XBL3.0 x=fh2mp;<decimal xuid>".
func (c *Core) xblXuid(r *Req, h http.Header) string {
	if r.Authed {
		return fmt.Sprint(r.Xuid) // from the session token, never from a header the client wrote
	}
	if c.Auth != nil && c.Auth.Mode == "required" {
		return ""
	}
	if m := xuidTokenRe.FindStringSubmatch(h.Get("Authorization")); m != nil {
		return m[1]
	}
	return ""
}

func splitPath(p string) []string {
	var out []string
	for _, s := range strings.Split(p, "/") {
		if s != "" {
			out = append(out, s)
		}
	}
	return out
}

func eqSegs(a []string, b ...string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// xblSocial answers an XSAPI profile / social / presence / peoplehub call: the status and the JSON object, or ok=false when the path is not one of ours.
func (c *Core) xblSocial(r *Req, host, command, path string, body []byte, h http.Header) (status int, out any, ok bool) {
	u, err := url.Parse(path)
	if err != nil {
		return 0, nil, false
	}
	q := u.Query()
	segs := splitPath(u.Path)
	var settings []string
	for _, v := range q["settings"] {
		for _, x := range strings.Split(v, ",") {
			if x != "" {
				settings = append(settings, x)
			}
		}
	}
	users := func(xuids []int64, wanted []string) any {
		list := []any{}
		for _, x := range xuids {
			u, err := social.ProfileUser(c.Social, x, wanted, picURL)
			if err != nil {
				panic(err)
			}
			list = append(list, u)
		}
		return list
	}
	friends := func(me int64) []int64 {
		f, err := c.Social.Friends(me)
		if err != nil {
			panic(err)
		}
		return f
	}
	switch host {
	case "profile.xboxlive.com":
		switch {
		case command == "POST" && eqSegs(segs, "users", "batch", "profile", "settings"):
			var ids []int64
			var wanted []string
			if len(body) > 0 {
				v, err := pyjson.Parse(string(body))
				if err != nil {
					panic(err)
				}
				o, _ := v.(pyjson.Obj)
				l, _ := o.Get("userIds")
				for _, x := range anyList(l) {
					ids = append(ids, toInt(x))
				}
				s, _ := o.Get("settings")
				for _, x := range anyList(s) {
					wanted = append(wanted, pyjson.PyStr(x))
				}
			}
			short := make([]string, len(ids))
			for i, id := range ids {
				short[i] = lastN(fmt.Sprint(id), 5)
			}
			c.Debug(r, "social", fmt.Sprintf("  profile settings for %d user(s): %s", len(ids), strings.Join(short, ",")))
			return 200, pyjson.Obj{{K: "profileUsers", V: users(ids, wanted)}}, true
		case command == "GET" && len(segs) == 4 && segs[0] == "users" && segs[2] == "profile" && segs[3] == "settings":
			return 200, pyjson.Obj{{K: "profileUsers", V: users([]int64{c.xuidOf(r, segs[1], h)}, settings)}}, true
		case command == "GET" && len(segs) == 6 && segs[0] == "users" && segs[2] == "profile" && segs[3] == "settings" && segs[4] == "people":
			me := c.xuidOf(r, segs[1], h)
			f := friends(me)
			c.Debug(r, "social", fmt.Sprintf("  profile settings of the social group %s of %#x: %d friends", pyjson.PyRepr(segs[5]), me, len(f)))
			return 200, pyjson.Obj{{K: "profileUsers", V: users(f, settings)}}, true
		}
	case "social.xboxlive.com":
		if command == "GET" && len(segs) == 3 && segs[0] == "users" {
			me := c.xuidOf(r, segs[1], h)
			switch segs[2] {
			case "people":
				p, err := social.People(c.Social, me)
				if err != nil {
					panic(err)
				}
				c.Debug(r, "social", fmt.Sprintf("  social people of %#x: %d", me, len(friends(me))))
				return 200, p, true
			case "summary":
				s, err := social.Summary(c.Social, me)
				if err != nil {
					panic(err)
				}
				return 200, s, true
			}
		}
	case "peoplehub.xboxlive.com":
		if command == "GET" && len(segs) >= 1 && segs[0] == "users" && len(segs) >= 3 && segs[2] == "people" {
			me := c.xuidOf(r, segs[1], h)
			out := []any{}
			for _, f := range friends(me) {
				prof, err := c.Social.Profile(f)
				if err != nil {
					panic(err)
				}
				state := "Offline"
				if social.Online(f) {
					state = "Online"
				}
				out = append(out, pyjson.Obj{{K: "xuid", V: fmt.Sprint(f)}, {K: "isFavorite", V: false}, {K: "isFollowingCaller", V: true}, {K: "isFollowedByCaller", V: true},
					{K: "displayName", V: prof.GameDisplayName}, {K: "realName", V: ""}, {K: "displayPicRaw", V: picURL(f, prof.PicVersion)}, {K: "gamertag", V: prof.Gamertag},
					{K: "gamerScore", V: prof.Gamerscore}, {K: "presenceState", V: state}})
			}
			c.Debug(r, "social", fmt.Sprintf("  peoplehub people of %#x: %d", me, len(out)))
			return 200, pyjson.Obj{{K: "people", V: out}}, true
		}
	case "userpresence.xboxlive.com":
		switch {
		case command == "POST" && eqSegs(segs, "users", "batch"):
			var ids []int64
			if len(body) > 0 {
				v, err := pyjson.Parse(string(body))
				if err != nil {
					panic(err)
				}
				o, _ := v.(pyjson.Obj)
				l, _ := o.Get("users")
				for _, x := range anyList(l) {
					ids = append(ids, toInt(x))
				}
			}
			c.Debug(r, "social", fmt.Sprintf("  presence of %d user(s)", len(ids)))
			list := []any{}
			for _, x := range ids {
				list = append(list, social.Presence(x))
			}
			return 200, list, true
		case command == "GET" && len(segs) == 3 && segs[0] == "users" && segs[2] == "presence":
			return 200, social.Presence(c.xuidOf(r, segs[1], h)), true
		}
	}
	return 0, nil, false
}

func lastN(s string, n int) string {
	if len(s) > n {
		return s[len(s)-n:]
	}
	return s
}

func anyList(v any) []any { l, _ := v.([]any); return l }

// toInt is int(x) for a JSON number or numeric text.
func toInt(v any) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case int:
		return int64(x)
	case uint64:
		return int64(x)
	case float64:
		return int64(x)
	case string:
		n, err := strconv.ParseInt(strings.TrimSpace(x), 10, 64)
		if err != nil {
			panic(&valErr{fmt.Sprintf("invalid literal for int() with base 10: %s", pyjson.PyRepr(x))})
		}
		return n
	}
	panic(&typeErr{fmt.Sprintf("int() argument must be a string, a bytes-like object or a real number, not '%s'", pyjson.TypeName(v))})
}

// playerRef is int(v, 0) for text, int(v) otherwise.
func playerRef(v any) (int64, error) {
	if s, ok := v.(string); ok {
		return parseIntBase0(s)
	}
	return toInt(v), nil
}

type apiResult struct {
	status int
	obj    any
	raw    []byte
}

// socialAPI is the admin / launcher API for friends and gamerpics (loopback only):
//
//	GET    /api/players                       known players with their friend count
//	GET    /api/friends/<xuid>                a player's friends
//	POST   /api/friends                       {"a": xuid, "b": xuid}  make two players friends; DELETE removes
//	PUT    /api/players/<xuid>/gamerpic       body: a PNG (<= 1 MB); DELETE removes it; GET returns it
//	POST   /api/players/<xuid>                {"displayName": "...", "gamerscore": n, "tier": "Gold"}
func (c *Core) socialAPI(r *Req, method, path string, body []byte) (res *apiResult) {
	rawPath, _, _ := strings.Cut(path, "?")
	parts := splitPath(rawPath)
	defer func() {
		if p := recover(); p != nil {
			e, ok := p.(error)
			if !ok {
				panic(p)
			}
			if isBadRequest(e) {
				head := strings.Join(parts[:min(2, len(parts))], "/")
				c.Warn(r, fmt.Sprintf("  admin api refused 400: %s %s/... : %s", method, head, pyWhere(e)))
				res = &apiResult{status: 400, obj: pyjson.Obj{{K: "error", V: e.Error()}}}
				return
			}
			panic(p)
		}
	}()
	must := func(err error) {
		if err != nil {
			panic(err)
		}
	}
	jsonBody := func() pyjson.Obj {
		if len(body) == 0 {
			return pyjson.Obj{}
		}
		v, err := pyjson.Parse(string(body))
		if err != nil {
			panic(&valErr{err.Error()})
		}
		o, isObj := v.(pyjson.Obj)
		if !isObj {
			panic(&typeErr{"list indices must be integers or slices, not str"})
		}
		return o
	}
	key := func(o pyjson.Obj, k string) any {
		v, ok := o.Get(k)
		if !ok {
			panic(&keyErr{k})
		}
		return v
	}
	ref := func(v any) int64 {
		n, err := playerRef(v)
		if err != nil {
			panic(&valErr{err.Error()})
		}
		return n
	}
	pint := func(s string) int64 {
		n, err := parseIntBase0(s)
		if err != nil {
			panic(&valErr{err.Error()})
		}
		return n
	}
	if len(parts) >= 2 && parts[0] == "api" && parts[1] == "players" && len(parts) == 2 && method == "GET" {
		rows, err := c.Social.Players()
		must(err)
		list := []any{}
		for _, p := range rows {
			list = append(list, pyjson.Obj{{K: "xuid", V: p.Xuid}, {K: "gamertag", V: p.Gamertag}, {K: "level", V: db.DisplayLevel(p.CareerLevel)}, {K: "friends", V: p.Friends},
				{K: "hasGamerpic", V: p.HasGamerpic}})
		}
		return &apiResult{status: 200, obj: pyjson.Obj{{K: "players", V: list}}}
	}
	if len(parts) >= 2 && parts[0] == "api" && parts[1] == "friends" {
		if len(parts) == 3 && method == "GET" {
			fr, err := c.Social.Friends(pint(parts[2]))
			must(err)
			list := []any{}
			for _, f := range fr {
				list = append(list, pyjson.Obj{{K: "xuid", V: f}, {K: "gamertag", V: c.tagFromDB(f)}})
			}
			return &apiResult{status: 200, obj: pyjson.Obj{{K: "friends", V: list}}}
		}
		if len(parts) == 2 && (method == "POST" || method == "DELETE") {
			d := jsonBody()
			a, b := ref(key(d, "a")), ref(key(d, "b"))
			if method == "POST" {
				must(c.Social.AddFriend(a, b))
				return &apiResult{status: 201, obj: pyjson.Obj{{K: "friends", V: []any{a, b}}}}
			}
			removed, err := c.Social.RemoveFriend(a, b)
			must(err)
			return &apiResult{status: 200, obj: pyjson.Obj{{K: "removed", V: removed}}}
		}
	}
	if len(parts) >= 3 && parts[0] == "api" && parts[1] == "players" {
		xuid := pint(parts[2])
		if len(parts) == 4 && parts[3] == "gamerpic" {
			switch method {
			case "PUT":
				must(c.Social.SetGamerpic(xuid, body))
				return &apiResult{status: 200, obj: pyjson.Obj{{K: "updated", V: xuid}, {K: "bytes", V: len(body)}}}
			case "DELETE":
				must(c.Social.ClearGamerpic(xuid))
				return &apiResult{status: 200, obj: pyjson.Obj{{K: "cleared", V: xuid}}}
			case "GET":
				png, err := c.Social.Gamerpic(xuid)
				must(err)
				return &apiResult{status: 200, raw: png, obj: nil}
			}
		}
		if len(parts) == 3 && method == "POST" {
			d := jsonBody()
			var name, tier *string
			var score *int64
			if v, ok := d.Get("displayName"); ok && v != nil {
				s := pyjson.PyStr(v)
				name = &s
			}
			if v, ok := d.Get("gamerscore"); ok && v != nil {
				n := toInt(v)
				score = &n
			}
			if v, ok := d.Get("tier"); ok && v != nil {
				s := pyjson.PyStr(v)
				tier = &s
			}
			must(c.Social.SetProfile(xuid, name, score, tier))
			return &apiResult{status: 200, obj: pyjson.Obj{{K: "updated", V: xuid}}}
		}
	}
	return nil
}

type keyErr struct{ k string }

func (e *keyErr) Error() string { return pyjson.PyRepr(e.k) }

type typeErr struct{ m string }

func (e *typeErr) Error() string { return e.m }

type valErr struct{ m string }

func (e *valErr) Error() string { return e.m }

// isBadRequest: the errors Python's `except (ValueError, KeyError, TypeError)` catches.
func isBadRequest(e error) bool {
	switch e.(type) {
	case *social.ValueError, *keyErr, *typeErr, *valErr:
		return true
	}
	return false
}
