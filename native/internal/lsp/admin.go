package lsp

import (
	"encoding/base64"
	"fmt"
	"net/url"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"fh2mp/internal/capacity"
	"fh2mp/internal/drivatar"
	"fh2mp/internal/notifstore"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/rivals"
	"fh2mp/internal/xls"
)

// The JSON admin API for notifications (/api/*, loopback only), the /admin/* operator endpoints, and the answers for the Xbox Live hosts the game
// talks to (game server directory, matchmaking cluster, string validation).

// ApiError is an error the admin API answers with its status.
type ApiError struct {
	Status int
	Msg    string
}

func (e *ApiError) Error() string { return e.Msg }

func apiErr(status int, f string, a ...any) *ApiError { return &ApiError{status, fmt.Sprintf(f, a...)} }

var sTypes = map[int64]string{0: "u8", 1: "u16", 2: "i16", 3: "i32", 4: "u32", 5: "i64", 6: "u64", 7: "float", 9: "string", 0x10: "datetime", 0x11: "bool"}

func (c *Core) fieldInfo(ntype int64) []any {
	out := []any{}
	for _, f := range c.notifFields(ntype) {
		t, ok := sTypes[f.Type]
		if !ok {
			t = fmt.Sprintf("%#x", f.Type)
		}
		var ml any
		if f.Size != 0 {
			ml = int64(f.Size)
		}
		out = append(out, pyjson.Obj{{K: "name", V: f.Name}, {K: "type", V: t}, {K: "maxLength", V: ml}})
	}
	return out
}

func (c *Core) notifJSON(n *notifstore.Notification) pyjson.Obj {
	var kind any
	for _, k := range kinds {
		if k.ntype == n.Type {
			kind = k.name
			break
		}
	}
	params := n.Params
	if params == nil {
		params = pyjson.Obj{}
	}
	readBy := n.ReadBy
	if readBy == nil {
		readBy = []int64{}
	}
	secs := float64(n.Sent)/1e7 - 11644473600
	return pyjson.Obj{{K: "id", V: n.ID}, {K: "type", V: n.Type}, {K: "kind", V: kind}, {K: "text", V: n.Message}, {K: "sra", V: n.Sra}, {K: "fields", V: params},
		{K: "xuid", V: n.Xuid}, {K: "read", V: n.Read}, {K: "readBy", V: readBy}, {K: "sent", V: time.Unix(int64(secs), 0).UTC().Format("2006-01-02T15:04:05Z")}}
}

// Api serves the notification admin API:
//
//	GET    /api/types                 notification kinds, categories and each type's fields
//	GET    /api/notifications         everything queued
//	POST   /api/notifications         {"kind"|"type", "text", "sra", "xuid", "fields": {...}} -> created item
//	GET    /api/notifications/<id>;  DELETE /api/notifications/<id>;  DELETE /api/notifications  remove all
func (c *Core) Api(r *Req, method, path string, body []byte) (int, any, error) {
	rawPath, _, _ := strings.Cut(path, "?")
	parts := splitPath(rawPath)
	if len(parts) >= 2 && parts[0] == "api" && parts[1] == "types" && method == "GET" {
		ks := []any{}
		inKinds := map[int64]bool{}
		for _, k := range kinds {
			inKinds[k.ntype] = true
			ks = append(ks, pyjson.Obj{{K: "kind", V: k.name}, {K: "type", V: k.ntype}, {K: "shows_in", V: k.where}, {K: "fields", V: c.fieldInfo(k.ntype)}})
		}
		var known []int64
		for t := range notifStructs {
			if !inKinds[t] {
				known = append(known, t)
			}
		}
		sort.Slice(known, func(i, j int) bool { return known[i] < known[j] })
		other := []any{}
		for _, t := range known {
			other = append(other, pyjson.Obj{{K: "type", V: t}, {K: "fields", V: c.fieldInfo(t)}})
		}
		cats := []any{}
		for _, ct := range categories {
			cats = append(cats, pyjson.Obj{{K: "id", V: ct.id}, {K: "name", V: ct.name}, {K: "description", V: ct.desc}})
		}
		return 200, pyjson.Obj{{K: "kinds", V: ks}, {K: "otherTypes", V: other}, {K: "categories", V: cats}}, nil
	}
	if len(parts) < 2 || parts[0] != "api" || parts[1] != "notifications" {
		return 0, nil, apiErr(404, "unknown endpoint")
	}
	if len(parts) == 2 {
		switch method {
		case "GET":
			all, err := c.Notif.All()
			if err != nil {
				return 0, nil, err
			}
			list := []any{}
			for _, n := range all {
				list = append(list, c.notifJSON(n))
			}
			return 200, pyjson.Obj{{K: "notifications", V: list}}, nil
		case "DELETE":
			n, err := c.Notif.DeleteAll()
			if err != nil {
				return 0, nil, err
			}
			return 200, pyjson.Obj{{K: "removed", V: n}}, nil
		case "POST":
			return c.apiCreate(r, body)
		}
		return 0, nil, apiErr(405, "method not allowed")
	}
	nid, err := strconv.ParseInt(strings.TrimSpace(parts[2]), 10, 64)
	if err != nil {
		return 0, nil, apiErr(404, "bad notification id")
	}
	n, err := c.Notif.Get(nid)
	if err != nil {
		return 0, nil, err
	}
	if n == nil {
		return 0, nil, apiErr(404, "no such notification")
	}
	switch method {
	case "GET":
		return 200, c.notifJSON(n), nil
	case "DELETE":
		if _, err := c.Notif.Delete(nid); err != nil {
			return 0, nil, err
		}
		return 200, pyjson.Obj{{K: "removed", V: nid}}, nil
	}
	return 0, nil, apiErr(405, "method not allowed")
}

func (c *Core) apiCreate(r *Req, body []byte) (int, any, error) {
	var d pyjson.Obj
	if len(body) > 0 {
		v, err := pyjson.Parse(string(body))
		if err != nil {
			return 0, nil, apiErr(400, "invalid JSON: %v", err)
		}
		d, _ = v.(pyjson.Obj)
	}
	var ntype int64
	if kv, ok := d.Get("kind"); ok {
		name := pyjson.PyStr(kv)
		found := false
		for _, k := range kinds {
			if k.name == name {
				ntype, found = k.ntype, true
			}
		}
		if !found {
			var names []string
			for _, k := range kinds {
				names = append(names, k.name)
			}
			sort.Strings(names)
			l := make([]any, len(names))
			for i, n := range names {
				l[i] = n
			}
			return 0, nil, apiErr(400, "unknown kind %s; have %s", pyjson.PyRepr(kv), pyjson.PyRepr(l))
		}
	} else if tv, ok := d.Get("type"); ok {
		var err error
		if s, isStr := tv.(string); isStr {
			ntype, err = parseIntBase0(s)
		} else {
			ntype, err = toIntErr(tv)
		}
		if err != nil {
			return 0, nil, apiErr(400, "type must be a number")
		}
		if _, ok := notifStructs[ntype]; !ok {
			return 0, nil, apiErr(400, "type %#x is not dispatched by the client", ntype)
		}
	} else {
		return 0, nil, apiErr(400, "give \"kind\" or \"type\"")
	}
	var fields pyjson.Obj
	if fv, ok := d.Get("fields"); ok {
		if fo, isObj := fv.(pyjson.Obj); isObj {
			fields = append(pyjson.Obj(nil), fo...)
		}
	}
	var carID *int64
	if cv, ok := fields.Get("carId"); ok && ntype == 0x8502 { // convenience: look make/model up in the game database
		id, err := toIntErr(cv)
		if err != nil {
			return 0, nil, err
		}
		fields = delKey(fields, "carId")
		carID = &id
		mk, md, ok, err := c.Cars.MakeModel(id)
		if err != nil {
			return 0, nil, err
		}
		if !ok {
			return 0, nil, apiErr(400, "unknown carId %d (not in the game database)", id)
		}
		if _, has := fields.Get("MakeId"); !has {
			fields.Set("MakeId", mk)
		}
		if _, has := fields.Get("ModelId"); !has {
			fields.Set("ModelId", md)
		}
	}
	gv, _ := fields.Get("GiftId")
	isGift := (ntype == 0x8500 || ntype == 0x8501 || ntype == 0x8502) && !truthy(gv) // GiftId = the new id, set on insert
	params, err := c.notifParams(ntype, fields)
	if err != nil {
		return 0, nil, apiErr(400, "%s", err.Error())
	}
	var xuid int64
	if xv, ok := d.Get("xuid"); ok {
		if s, isStr := xv.(string); isStr {
			xuid, err = parseIntBase0(s)
		} else {
			xuid, err = toIntErr(xv)
		}
		if err != nil {
			return 0, nil, apiErr(400, "%s", err.Error())
		}
	}
	if _, ok := d.Get("text"); ok {
		has := false
		for _, f := range c.notifFields(ntype) {
			has = has || f.Name == "Message"
		}
		if !has {
			return 0, nil, apiErr(400, "type %#x has no message text; set \"fields\" instead", ntype)
		}
	}
	text, sra := "", ""
	if v, ok := d.Get("text"); ok {
		text = pyjson.PyStr(v)
	}
	if v, ok := d.Get("sra"); ok {
		sra = pyjson.PyStr(v)
	}
	n, err := c.AddNotification(r, text, sra, ntype, params, xuid, carID, isGift)
	if err != nil {
		return 0, nil, err
	}
	return 201, c.notifJSON(n), nil
}

func delKey(o pyjson.Obj, k string) pyjson.Obj {
	var out pyjson.Obj
	for _, p := range o {
		if p.K != k {
			out = append(out, p)
		}
	}
	return out
}

func truthy(v any) bool {
	switch x := v.(type) {
	case nil:
		return false
	case bool:
		return x
	case int64:
		return x != 0
	case uint64:
		return x != 0
	case float64:
		return x != 0
	case string:
		return x != ""
	case []any:
		return len(x) > 0
	case pyjson.Obj:
		return len(x) > 0
	}
	return true
}

func toIntErr(v any) (n int64, err error) {
	defer func() {
		if p := recover(); p != nil {
			if e, ok := p.(error); ok {
				err = e
				return
			}
			panic(p)
		}
	}()
	return toInt(v), nil
}

// ---------------------------------------------------------------- /admin/*

var carToken = regexp.MustCompile(`\{car:(\d+)\}`)

// expandCarTokens is expand_car_tokens: "{car:2101}" in an /admin/send text (the game server only knows car ids) becomes "Make Model (2101)",
// or just "2101" for a car the catalog lacks.
func (c *Core) expandCarTokens(text string) (string, error) {
	var ferr error
	out := carToken.ReplaceAllStringFunc(text, func(tok string) string {
		id, err := strconv.ParseInt(carToken.FindStringSubmatch(tok)[1], 10, 64)
		if err != nil {
			return tok
		}
		row, err := c.DB.One("SELECT make, name FROM game.v_cars WHERE id=?", id)
		if err != nil {
			ferr = err
			return tok
		}
		name := ""
		if row != nil {
			name = strings.TrimSpace(row.Str("make") + " " + row.Str("name"))
		}
		if name == "" {
			return strconv.FormatInt(id, 10)
		}
		return fmt.Sprintf("%s (%d)", name, id)
	})
	return out, ferr
}

// Admin serves GET /admin/send, /admin/gamemode, /admin/drivatar, /admin/rotate, /admin/rivals and POST /admin/result: the text answered (200,
// text/plain), or found=false for a path that is not an admin endpoint.
func (c *Core) Admin(r *Req, method, rawurl string, body []byte) (out string, found bool, err error) {
	u, perr := url.Parse(rawurl)
	if perr != nil {
		return "", false, nil
	}
	q := map[string]string{}
	for k, v := range u.Query() {
		if len(v) > 0 {
			q[k] = v[0]
		}
	}
	has := func(k string) bool { _, ok := q[k]; return ok }
	p0 := func(k, d string) int64 {
		s, ok := q[k]
		if !ok {
			s = d
		}
		n, e := parseIntBase0(s)
		if e != nil {
			panic(&valErr{e.Error()})
		}
		return n
	}
	switch {
	case u.Path == "/admin/gamemode":
		typ := int64(1)
		if s, ok := q["type"]; ok {
			n, e := strconv.ParseInt(strings.TrimSpace(s), 10, 64)
			if e != nil {
				panic(&valErr{fmt.Sprintf("invalid literal for int() with base 10: %s", pyjson.PyRepr(s))})
			}
			typ = n
		}
		var xs []string
		for _, x := range strings.Split(q["xuids"], ",") {
			if x != "" {
				xs = append(xs, x)
			}
		}
		name, template := c.Xbl.CreateGameSession(int(typ), xs)
		txt, _ := pyjson.Dumps(pyjson.Obj{{K: "name", V: name}, {K: "template", V: template}}, pyjson.Opts{})
		td, ok := q["type"]
		if !ok {
			td = "1"
		}
		c.Log(r, fmt.Sprintf("  game-mode session created: %s (%s) type %s", name, template, td))
		return txt + "\n", true, nil
	case u.Path == "/admin/send":
		fixed := map[string]bool{"text": true, "sra": true, "type": true, "xuid": true}
		ntype := int64(0x200)
		if has("type") {
			ntype = p0("type", "")
		}
		var extra pyjson.Obj
		keys := make([]string, 0, len(q))
		for k := range q {
			keys = append(keys, k)
		}
		for _, p := range orderedQuery(u.RawQuery) {
			if !fixed[p.K] {
				extra = append(extra, p)
			}
		}
		_ = keys
		params, e := c.notifParams(ntype, extra)
		if e != nil {
			panic(&valErr{e.Error()})
		}
		var xuid int64
		if has("xuid") {
			xuid = p0("xuid", "")
		}
		text, e := c.expandCarTokens(q["text"])
		if e != nil {
			return "", true, e
		}
		n, e := c.AddNotification(r, text, q["sra"], ntype, params, xuid, nil, false)
		if e != nil {
			return "", true, e
		}
		return fmt.Sprintf("queued #%d\n", n.ID), true, nil
	case u.Path == "/admin/drivatar" && has("remove"):
		ok, e := c.Drv.Remove(p0("xuid", "0"))
		if e != nil {
			return "", true, e
		}
		if ok {
			return "removed\n", true, nil
		}
		return "no such drivatar\n", true, nil
	case u.Path == "/admin/drivatar" && (has("set") || has("lock")):
		var sets []drivatar.Set
		for _, kv := range strings.Split(q["set"], ",") {
			if kv == "" {
				continue
			}
			k, v, ok := strings.Cut(kv, "=")
			if !ok {
				panic(&valErr{"dictionary update sequence element #0 has length 1; 2 is required"})
			}
			f, e := strconv.ParseFloat(strings.TrimSpace(v), 64)
			if e != nil {
				panic(&valErr{fmt.Sprintf("could not convert string to float: %s", pyjson.PyRepr(v))})
			}
			sets = append(sets, drivatar.Set{K: k, V: f})
		}
		var lock *bool
		if has("lock") {
			l := q["lock"] != "0" && q["lock"] != "false"
			lock = &l
		}
		rec, e := c.Drv.Edit(p0("xuid", "0x9000000000000"), sets, lock)
		if e != nil {
			return "", true, e
		}
		if rec == nil {
			return "no such drivatar\n", true, nil
		}
		ui, _ := rec.Get("UserInfo")
		uo, _ := ui.(pyjson.Obj)
		num := func(k string) float64 { v, _ := uo.Get(k); f, _ := v.(float64); return f }
		locked := false
		if lv, ok := rec.Get("_locked"); ok {
			locked = truthy(lv)
		}
		tag, _ := rec.Get("GamerTag")
		skill, _ := rec.Get("UserSkillLevel")
		return fmt.Sprintf("%s locked=%s sideswipe=%.3f temperament=%.3f blocking=%.3f skill=%s\n", pyjson.PyRepr(tag), pyBool(locked), num("sideswipeMean"),
			num("temperamentMean"), num("blockingMean"), pyjson.PyStr(skill)), true, nil
	case u.Path == "/admin/drivatar":
		var skill, wrist *int64
		if has("skill") {
			s := p10(q["skill"])
			skill = &s
		}
		if has("wristband") {
			w := p10(q["wristband"])
			wrist = &w
		}
		tag := "FH2MP Guest"
		if has("tag") {
			tag = q["tag"]
		}
		rec, e := c.Drv.AddPlayer(p0("xuid", "0x9000000000001"), tag, skill, wrist)
		if e != nil {
			return "", true, e
		}
		x, _ := rec.Get("Xuid")
		gt, _ := rec.Get("GamerTag")
		sk, _ := rec.Get("UserSkillLevel")
		wb, _ := rec.Get("wristbandLevel")
		return fmt.Sprintf("drivatar %s xuid=%#x skill=%s wristband=%s\n", pyjson.PyRepr(gt), toInt(x), pyjson.PyStr(sk), pyjson.PyStr(wb)), true, nil
	case u.Path == "/admin/rotate" || u.Path == "/admin/rivals":
		var evs []*rivals.Event
		var e error
		if u.Path == "/admin/rotate" {
			evs, e = c.Riv.Rotate()
		} else {
			evs, e = c.Riv.Events()
		}
		if e != nil {
			return "", true, e
		}
		if e := c.announceRotation(r); e != nil {
			return "", true, e
		}
		var b strings.Builder
		for _, ev := range evs {
			fmt.Fprintf(&b, "#%d track %d %s %s :: %s\n", ev.ID, ev.Track, ev.Kind, pyjson.PyRepr(ev.Attrs), ev.Name)
		}
		return b.String(), true, nil
	}
	return "", false, nil
}

func p10(s string) int64 {
	n, e := strconv.ParseInt(strings.TrimSpace(s), 10, 64)
	if e != nil {
		panic(&valErr{fmt.Sprintf("invalid literal for int() with base 10: %s", pyjson.PyRepr(s))})
	}
	return n
}

// orderedQuery is the query's first value per key in the order the keys first appear (Python's dict of parse_qs).
func orderedQuery(raw string) pyjson.Obj {
	var out pyjson.Obj
	seen := map[string]bool{}
	for _, part := range strings.Split(raw, "&") {
		if part == "" {
			continue
		}
		k, v, _ := strings.Cut(part, "=")
		kk, err1 := url.QueryUnescape(k)
		vv, err2 := url.QueryUnescape(v)
		if err1 != nil || err2 != nil || v == "" || seen[kk] {
			continue // parse_qs drops blank values
		}
		seen[kk] = true
		out = append(out, pyjson.Pair{K: kk, V: vv})
	}
	return out
}

// ---------------------------------------------------------------- Xbox Live hosts

// GSIVersion is the build version string in the exe (PTR_s_6CC152FD); the same value as the NetV2 handshake.
const GSIVersion = "6CC152FD"

// maxLobbyPlayers is 12 unless FH2MP_WORLD_CAPACITY says more (capacity); docs/matchmaking.md.
func maxLobbyPlayers() int { return capacity.FreeRoam() }

// gsdsVariants answers POST gameserverds.xboxlive.com/titles/<tid>/variants (Xbox GSDS, GetVariants in the exe). Every field is a JSON *string*. The game
// matches the GSI set names against a regex, by default `Release_<version>\..*` (FUN_141838d70), and takes the "initialize failed" exit of the
// matchmaking-init state when none matches, so the sets are named Release_<version>.<mode>.
func gsdsVariants() []byte {
	schema := "00000000-0000-0000-0000-000000000002"
	modes := []string{"FreeRoam", "CarMeet", "RoadTrip"}
	var variants, sets []any
	for i, m := range modes {
		variants = append(variants, pyjson.Obj{{K: "gamevariantId", V: fmt.Sprintf("00000000-0000-0000-0000-00000000000%d", i+1)}, {K: "name", V: fmt.Sprintf("Release_%s.%s", GSIVersion, m)},
			{K: "isPublisher", V: "True"}, {K: "rank", V: "0"}, {K: "gameVariantSchemaId", V: schema}})
		sets = append(sets, pyjson.Obj{{K: "minRequiredPlayers", V: "1"}, {K: "maxAllowedPlayers", V: fmt.Sprint(maxLobbyPlayers())},
			{K: "gsiSetId", V: fmt.Sprintf("00000000-0000-0000-0000-00000000010%d", i)}, {K: "gsiSetName", V: fmt.Sprintf("Release_%s.%s", GSIVersion, m)},
			{K: "selectionOrder", V: fmt.Sprint(i)}, {K: "variantSchemaId", V: schema}})
	}
	doc := pyjson.Obj{{K: "variants", V: variants},
		{K: "variantSchemas", V: []any{pyjson.Obj{{K: "variantSchemaId", V: schema}, {K: "schemaContent", V: "{}"}, {K: "name", V: "FreeRoam"}}}},
		{K: "gsiSets", V: sets}}
	s, _ := pyjson.Dumps(doc, pyjson.Opts{})
	return []byte(s)
}

func publicHost() string {
	if h := os.Getenv("FH2MP_PUBLIC_HOST"); h != "" {
		return h
	}
	return "127.0.0.1"
}

// gsdsQosServers answers GET gameserverds.xboxlive.com/titles/<tid>/qosservers. The list must not be empty: XSAPI then returns a null vector from its QoS
// async and the game dereferences it (crash at game+0x41fd28, docs/matchmaking.md).
func gsdsQosServers() []byte {
	sda := base64.StdEncoding.EncodeToString([]byte("FH2MP-SDA-0000000"))
	doc := pyjson.Obj{{K: "qosServers", V: []any{pyjson.Obj{{K: "serverFqdn", V: publicHost()}, {K: "serverSecureDeviceAddress", V: sda}, {K: "targetLocation", V: "North Central US"}}}}}
	s, _ := pyjson.Dumps(doc, pyjson.Opts{})
	return []byte(s)
}

// gsmsCluster answers POST gameserverms.xboxlive.com/titles/<tid>/clusters (AllocateClusterAsync) with a ClusterResult: every allocation is answered at
// once with this server. Value types were checked in Microsoft.Xbox.Services.dll (wrong type -> 0x83750005): pollIntervalMilliseconds / fulfillmentState /
// hostName / region / Key are strings, Value is an object whose external / internal are numbers.
func gsmsCluster() []byte {
	port := int64(5000)
	if s := os.Getenv("FH2MP_GAME_PORT"); s != "" {
		port = p10(s)
	}
	doc := pyjson.Obj{{K: "fulfillmentState", V: "fulfilled"}, {K: "pollIntervalMilliseconds", V: "1000"}, {K: "hostName", V: publicHost()}, {K: "region", V: "North Central US"},
		{K: "portMappings", V: []any{pyjson.Obj{{K: "Key", V: "NetV2"}, {K: "Value", V: pyjson.Obj{{K: "external", V: port}, {K: "internal", V: port}}}}}}}
	s, _ := pyjson.Dumps(doc, pyjson.Opts{})
	return []byte(s)
}

// clusterSessionID is the session a GSMS AllocateCluster request names: {"sessionId": "/serviceconfigs/<scid>/sessionTemplates/<template>/sessions/<name>"}.
func clusterSessionID(body []byte) string {
	v, err := pyjson.Parse(string(body))
	if err != nil {
		return ""
	}
	o, ok := v.(pyjson.Obj)
	if !ok {
		return ""
	}
	id, _ := o.Get("sessionId")
	s, _ := id.(string)
	return s
}

// verifyStrings: XSAPI uses `stringstoVerify` (lower-case t), with one result per input; the old spelling stays accepted.
func verifyStrings(body []byte) (pyjson.Obj, error) {
	v, err := pyjson.Parse(string(body))
	if err != nil {
		return nil, err
	}
	o, ok := v.(pyjson.Obj)
	if !ok {
		return nil, fmt.Errorf("expected an object")
	}
	sv, found := o.Get("stringstoVerify")
	if !found {
		sv, _ = o.Get("stringsToVerify")
	}
	list, isList := sv.([]any)
	if isList {
		for _, s := range list {
			if _, isStr := s.(string); !isStr {
				isList = false
			}
		}
	}
	if !isList {
		return nil, fmt.Errorf("expected an array of strings")
	}
	res := make([]any, len(list))
	for i := range res {
		res[i] = pyjson.Obj{{K: "resultCode", V: int64(0)}, {K: "offendingString", V: ""}}
	}
	return pyjson.Obj{{K: "verifyStringResult", V: res}}, nil
}

var _ = xls.NewStruct
