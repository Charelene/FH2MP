package lsp

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	enchex "encoding/hex"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"net/http"
	"os"
	"strings"
	"testing"

	"fh2mp/internal/auth"
	"fh2mp/internal/avatars"
	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
	"fh2mp/internal/pngtool"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/social"
	"fh2mp/internal/xbl"
)

// ---------------------------------------------------------------- a launcher API on a test database

const avatarIP = "127.0.0.1"

type avatarAccount struct {
	Name     string `json:"name"`
	Xuid     int64  `json:"xuid"`
	XuidHex  string `json:"xuidHex"`
	Gamertag string `json:"gamertag"`
	Token    string `json:"token"`
}

type avatarRig struct {
	t     *testing.T
	c     *Core
	l     *Launcher
	clock float64
	accts map[string]avatarAccount
}

// newAvatarRig is the launcher API on an empty database with these accounts signed in (their tokens are made up: a session row is written
// for each). The avatar store's clock is rig.clock.
func newAvatarRig(t *testing.T, accounts []avatarAccount) *avatarRig {
	t.Helper()
	t.Setenv("FH2MP_AUTH_SECRET", "avatar-test-secret")
	t.Setenv("FH2MP_RATE_SCALE", "1")
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(d.Close)
	svc, err := auth.NewService(d, "dev", "optional", "open", "exact")
	if err != nil {
		t.Fatal(err)
	}
	g := &avatarRig{t: t, clock: 1800000000, accts: map[string]avatarAccount{}}
	now := nowF()
	err = d.Do(func(tx *db.Tx) error {
		for i, a := range accounts {
			if _, err := tx.Exec("INSERT INTO accounts (xuid, pubkey, gamertag, gamertag_key, created, created_ip) VALUES (?,?,?,?,?,?)",
				a.Xuid, []byte{byte(i), 0xa7}, a.Gamertag, auth.FoldTag(a.Gamertag), now, avatarIP); err != nil {
				return err
			}
			h := sha256.Sum256([]byte(a.Token))
			if _, err := tx.Exec("INSERT INTO auth_sessions (token_hash, xuid, ip, client, created, last_used, expires, hard_expires) VALUES (?,?,?,?,?,?,?,?)",
				h[:], a.Xuid, avatarIP, "avatar-test", now, now, now+auth.SessionIdle, now+auth.SessionMax); err != nil {
				return err
			}
			g.accts[a.Name] = a
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	c := New()
	c.Out = func(string) {}
	c.DB, c.Auth, c.Social, c.Avatars = d, svc, social.New(d), avatars.New(d)
	c.Avatars.Now = func() float64 { return g.clock }
	c.Xbl = xbl.New(&lobbyHub{})
	g.c, g.l = c, NewLauncher(c)
	c.Launcher = g.l
	return g
}

// do sends one /v1 request as the named account ("" = without a token); a JSON answer comes back as its text.
func (g *avatarRig) do(as, method, path string, body []byte) (status int, text string, raw []byte, ctype string) {
	g.t.Helper()
	h := http.Header{}
	if as != "" {
		h.Set("Authorization", "Bearer "+g.accts[as].Token)
	}
	status, obj, ctype := g.l.Handle(&Req{}, method, path, h, body, avatarIP)
	if b, isBytes := obj.([]byte); isBytes {
		return status, "", b, ctype
	}
	s, err := pyjson.Dumps(obj, pyjson.Opts{})
	if err != nil {
		g.t.Fatal(err)
	}
	return status, s, nil, ctype
}

// put is PUT /v1/me/avatar with these fields (nil leaves a field out).
func (g *avatarRig) put(as string, manifest []byte, colours any, body []byte) (int, string) {
	g.t.Helper()
	d := pyjson.Obj{}
	if manifest != nil {
		d = append(d, pyjson.Pair{K: "manifest", V: base64.StdEncoding.EncodeToString(manifest)})
	}
	if colours != nil {
		d = append(d, pyjson.Pair{K: "colours", V: colours})
	}
	if body != nil {
		d = append(d, pyjson.Pair{K: "body", V: base64.StdEncoding.EncodeToString(body)})
	}
	s, _ := pyjson.Dumps(d, pyjson.Opts{})
	status, text, _, _ := g.do(as, "PUT", "/v1/me/avatar", []byte(s))
	return status, text
}

func (g *avatarRig) control(line string) string {
	g.t.Helper()
	out, found, err := g.c.Admin(&Req{}, "GET", line, nil)
	if err != nil || !found {
		g.t.Fatalf("%s: found %v, %v", line, found, err)
	}
	return out
}

// errorOf is the code of an error answer ("" for one that is not).
func errorOf(text string) string {
	v, err := pyjson.Parse(text)
	if err != nil {
		return ""
	}
	o, _ := v.(pyjson.Obj)
	e, _ := o.Get("error")
	s, _ := e.(string)
	return s
}

func jsonNum(t *testing.T, text string, path ...string) float64 {
	t.Helper()
	v, err := pyjson.Parse(text)
	if err != nil {
		t.Fatalf("%v: %s", err, text)
	}
	for _, k := range path {
		o, _ := v.(pyjson.Obj)
		v, _ = o.Get(k)
	}
	switch x := v.(type) {
	case float64:
		return x
	case int64:
		return float64(x)
	}
	t.Fatalf("%v in %s is %T", path, text, v)
	return 0
}

// ---------------------------------------------------------------- fixtures

var (
	avatarAnn = avatarAccount{"ann", 0x9000000000a01, "0x9000000000a01", "Avatar Ann", "fh2.avatar-contract-ann"}
	avatarBob = avatarAccount{"bob", 0x9000000000a02, "0x9000000000a02", "Avatar Bob", "fh2.avatar-contract-bob"}
	avatarCy  = avatarAccount{"cy", 0x9000000000a03, "0x9000000000a03", "Avatar Cy", "fh2.avatar-contract-cy"}

	// made once with Go's image/png and kept as they are, so the contract never depends on an encoder: a 64 x 128 RGBA figure on a clear
	// background, and a 32 x 32 grey square
	avatarBody64x128 = mustB64("iVBORw0KGgoAAAANSUhEUgAAAEAAAACACAYAAAC7gW9qAAABKElEQVR42uzbbxGCQBAHUI4xCBGIYASjEIUoRjACEWiiERiRvVvh/T47Mvd2lz83c3138QAAAAAAAAAA2mV9zu+Gl+9KxgUPj6mcDmBPpWtA9JnbvMZ49NlnPBrBUyBz9Wt0gQ4AACD3/EffB8IAjn6JiXopMgIAAMTlqLmN/CjSAdEX+LV60Z/EVTpg7yKiF9/ZEUqQ1nuCl8/t6D8cp1doRZf5Xv7uJpg5AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIDzJexw0jCMmydH1nUp3/5WBwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIDoAAIDNfAYA9Bs/N9UeVY0AAAAASUVORK5CYII=")
	avatarGrey32     = mustB64("iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAAAAABWESUoAAAAGUlEQVR42mJpYMAPmBhGFYwqGFUwQhUABgCkHgDDqP+neAAAAABJRU5ErkJggg==")

	avatarReward = mustItem("f0000000000000000000000000000001") // the test reward item the contract run puts in the catalog
)

func mustB64(s string) []byte {
	b, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		panic(err)
	}
	return b
}

func mustItem(s string) avatars.ItemID {
	id, err := avatars.ParseItem(s)
	if err != nil {
		panic(err)
	}
	return id
}

// headerOnlyPNG is a PNG signature, an IHDR for w x h (8-bit RGBA) and IEND: enough for the server to refuse it by its header.
func headerOnlyPNG(w, h uint32) []byte {
	var b bytes.Buffer
	b.WriteString("\x89PNG\r\n\x1a\n")
	hd := []byte{byte(w >> 24), byte(w >> 16), byte(w >> 8), byte(w), byte(h >> 24), byte(h >> 16), byte(h >> 8), byte(h), 8, 6, 0, 0, 0}
	chunk(&b, "IHDR", hd)
	chunk(&b, "IEND", nil)
	return b.Bytes()
}

func chunk(b *bytes.Buffer, t string, d []byte) {
	n := len(d)
	b.Write([]byte{byte(n >> 24), byte(n >> 16), byte(n >> 8), byte(n)})
	b.WriteString(t)
	b.Write(d)
	c := crc32IEEE(append([]byte(t), d...))
	b.Write([]byte{byte(c >> 24), byte(c >> 16), byte(c >> 8), byte(c)})
}

func crc32IEEE(p []byte) uint32 {
	c := ^uint32(0)
	for _, x := range p {
		c ^= uint32(x)
		for k := 0; k < 8; k++ {
			if c&1 != 0 {
				c = c>>1 ^ 0xedb88320
			} else {
				c >>= 1
			}
		}
	}
	return ^c
}

// avatarManifest is a made-up manifest (no real avatar's): a version word, chin, nose and ears ids, a mouth texture, body, head, two worn
// items in the stock id form ({mask}{index}{body}C1C8F109A19CB2E0, invented numbers), colours, and the extra ids given at worn slots 2...
func avatarManifest(extra ...avatars.ItemID) []byte {
	m := make([]byte, avatars.ManifestSize)
	put := func(o int, h string) {
		b, err := enchex.DecodeString(h)
		if err != nil {
			panic(err)
		}
		copy(m[o:], b)
	}
	stock := func(mask uint32, index, body uint16) string {
		return fmt.Sprintf("%08x%04x%04xc1c8f109a19cb2e0", mask, index, body)
	}
	put(0x000, "00000001")
	put(0x00C, stock(0x10000000, 0x0101, 1))
	put(0x01C, stock(0x20000000, 0x0102, 1))
	put(0x02C, stock(0x40000000, 0x0103, 1))
	put(0x03C, stock(0x01000000, 0x0201, 1))
	put(0x0FC, "ffe0b090ff302010ff3070c0ff202030ffffffffffffffffffffffffffffffffffffffff")
	put(0x120, stock(0x00000001, 0x0001, 1))
	put(0x140, stock(0x00000002, 0x0002, 1))
	put(0x160, stock(0x00000008, 0x0301, 1)+"0008")
	put(0x180, stock(0x00000010, 0x0302, 1)+"0010")
	for i, id := range extra {
		copy(m[0x1A0+32*i:], id[:])
	}
	return m
}

const avatarColours = "12 3\n407 1\n"

func pixelsOf(t *testing.T, b []byte) (*pngtool.Image, string) {
	t.Helper()
	im, err := pngtool.Decode(b, 4096)
	if err != nil {
		t.Fatalf("the PNG does not decode: %v", err)
	}
	h := sha256.New()
	for _, r := range im.Rows {
		h.Write(r)
	}
	return im, enchex.EncodeToString(h.Sum(nil))
}

// ---------------------------------------------------------------- the contract (native/testdata/avatar_api.json)

const avatarContractPath = "../../testdata/avatar_api.json"

type contractError struct {
	Status int    `json:"status"`
	Error  string `json:"error"`
	When   string `json:"when"`
}

type contractEndpoint struct {
	Method   string          `json:"method"`
	Path     string          `json:"path"`
	Auth     string          `json:"auth"`
	Request  json.RawMessage `json:"request,omitempty"`
	Response json.RawMessage `json:"response"`
	Errors   []contractError `json:"errors"`
	Notes    []string        `json:"notes,omitempty"`
}

type contractSlot struct {
	At    int    `json:"at"`
	AtHex string `json:"atHex"`
	Count int    `json:"count"`
	Step  int    `json:"step"`
	What  string `json:"what"`
}

type contractPNG struct {
	Width        int    `json:"width"`
	Height       int    `json:"height"`
	Channels     int    `json:"channels"`
	PixelsSha256 string `json:"pixelsSha256"`
}

type contractRequest struct {
	Method string          `json:"method"`
	Path   string          `json:"path"`
	As     string          `json:"as,omitempty"`
	JSON   json.RawMessage `json:"json,omitempty"`
}

type contractResponse struct {
	Status      int             `json:"status"`
	ContentType string          `json:"contentType"`
	Pick        []string        `json:"pick,omitempty"`
	JSON        json.RawMessage `json:"json,omitempty"`
	PNG         *contractPNG    `json:"png,omitempty"`
}

type contractStep struct {
	Note     string            `json:"note"`
	Clock    float64           `json:"clock,omitempty"`
	Repeat   int               `json:"repeat,omitempty"`
	Control  string            `json:"control,omitempty"`
	Answer   string            `json:"answer,omitempty"`
	Request  *contractRequest  `json:"request,omitempty"`
	Response *contractResponse `json:"response,omitempty"`
}

type contractFixture struct {
	Name         string `json:"name"`
	What         string `json:"what"`
	Base64       string `json:"base64"`
	PixelsSha256 string `json:"pixelsSha256,omitempty"`
}

type avatarContract struct {
	Contract  string          `json:"contract"`
	About     []string        `json:"about"`
	LimitsRaw json.RawMessage `json:"limits"`
	ItemID    struct {
		Text    string         `json:"text"`
		Empty   string         `json:"empty"`
		Slots   []contractSlot `json:"slots"`
		Worn    string         `json:"worn"`
		Rewards string         `json:"rewards"`
	} `json:"itemId"`
	Card      string             `json:"card"`
	Grants    []string           `json:"grants"`
	Endpoints []contractEndpoint `json:"endpoints"`
	Fixtures  []contractFixture  `json:"fixtures"`
	Run       struct {
		About    []string        `json:"about"`
		Accounts []avatarAccount `json:"accounts"`
		Rewards  []string        `json:"rewards"`
		Steps    []contractStep  `json:"steps"`
	} `json:"run"`
}

func rawJSON(o any) json.RawMessage {
	s, err := pyjson.Dumps(o, pyjson.Opts{})
	if err != nil {
		panic(err)
	}
	return json.RawMessage(s)
}

func putBody(manifest []byte, colours string, body []byte) json.RawMessage {
	d := pyjson.Obj{}
	if manifest != nil {
		d = append(d, pyjson.Pair{K: "manifest", V: base64.StdEncoding.EncodeToString(manifest)})
	}
	d = append(d, pyjson.Pair{K: "colours", V: colours})
	if body != nil {
		d = append(d, pyjson.Pair{K: "body", V: base64.StdEncoding.EncodeToString(body)})
	}
	return rawJSON(d)
}

// avatarContractDraft is the contract without its answers: what it says, and the run of requests whose answers the server fills in.
func avatarContractDraft(t *testing.T) *avatarContract {
	k := &avatarContract{Contract: "FH2MP avatar API 1"}
	k.About = []string{
		"The Xbox 360 avatar API of the FH2MP LSP for Encore: native/internal/lsp/avatar.go and native/internal/avatars.",
		"TestAvatarContract (native/internal/lsp/avatar_test.go) writes this file with FH2MP_WRITE_AVATAR_CONTRACT=1 and fails when the server no longer answers what it says. Encore keeps a copy in testdata/ and its interop lane checks the copy against FH2MP at proto/FH2MP_COMMIT.",
		"Base64 is the standard alphabet with padding (RFC 4648 section 4), checked strictly.",
		"Errors are {\"error\": <code>, \"message\": <text>} with the HTTP status. The code is for programs, the message for people. A request whose Content-Length is over the route's limit gets 413 {\"error\": \"too_large\"} before it is read.",
		"There is no store and no money: avatar items are rewards (later The Eliminator's battle pass), granted by the server only.",
	}
	k.LimitsRaw = rawJSON(pyjson.Obj{
		{K: "manifestBytes", V: int64(avatars.ManifestSize)},
		{K: "coloursMaxBytes", V: int64(avatars.ColoursMax)},
		{K: "coloursLines", V: `each line "<record> <palette>": two numbers of 1 to 6 digits, one space, ended by "\n"; "" or no colours field = no choices`},
		{K: "bodyMaxBytes", V: int64(avatarBodyMaxBytes)},
		{K: "bodyMinPixels", V: int64(avatarBodyMinDim)},
		{K: "bodyMaxPixels", V: int64(avatarBodyMaxDim)},
		{K: "bodyFormat", V: fmt.Sprintf("PNG, 8 bits per channel, grey, grey+alpha, RGB or RGBA, not interlaced; width and height each %d to %d pixels; at most %d bytes as sent", avatarBodyMinDim, avatarBodyMaxDim, avatarBodyMaxBytes)},
		{K: "bodyServed", V: "the server keeps and serves its own encoding of the pixels (pngtool), never the uploaded bytes: compare pictures by their pixels"},
		{K: "requestMaxBytes", V: int64(avatarRequestMax)},
		{K: "savesPer10Minutes", V: int64(avatarSavesPer10m)},
	})
	k.ItemID.Text = "32 lowercase hex digits: the item's 16 id bytes in the order the manifest holds them. The server reads either case and answers in lower case."
	k.ItemID.Empty = "16 zero bytes are an empty slot, not an item (nor can they be granted)."
	k.ItemID.Slots = []contractSlot{
		{avatars.FaceAt, fmt.Sprintf("0x%03X", avatars.FaceAt), avatars.FaceSlots, avatars.FaceStep,
			"face parts: chin, nose and ears (16 bytes each), then six 32-byte face textures (mouth, eyes, eyebrows, facial hair, eye shadow, facial features); read as fifteen 16-byte slots, so a texture's second half is read too"},
		{avatars.AssetAt, fmt.Sprintf("0x%03X", avatars.AssetAt), avatars.AssetSlots, avatars.AssetStep,
			"asset entries, the id in each entry's first 16 bytes: body, head, 13 worn items, 7 of the basic outfit (to 0x3E0)"},
	}
	k.ItemID.Worn = "A manifest wears every non-empty id in those slots. Nothing else in it is read as an id."
	k.ItemID.Rewards = fmt.Sprintf("The reward catalog lists the items that must be owned to be worn; every other item is free. It holds %d items in this build (none yet: the battle pass picks them). PUT refuses a manifest wearing a catalog item the account does not own.", len(avatars.Rewards))
	k.Card = "Every community card (GET /v1/community, /v1/community/player/<xuid>, invites, ...) carries avatarVersion next to gamerpicVersion: the time of the avatar's last save in seconds since 1970 (a float), above the one before at every save; 0 = no avatar. Fetch GET /v1/avatar/<xuid> again when it changes."
	k.Grants = []string{
		"Items are granted by the server only: avatars.Store.Grant(xuid, item, source) (the battle pass calls it later). No client request grants.",
		"For tests, the operators' control line (loopback only, like every /admin/ endpoint) answers one text line: GET /admin/avatar-items?xuid=<xuid>&grant=<item>[&source=<source>] grants (source \"admin\" when left out) and answers \"granted <item> to <xuid hex> (source <source>)\", \"<xuid hex> already owns <item>\" or \"refused: <reason>\"; GET /admin/avatar-items?xuid=<xuid> lists \"<item> granted <time> by <source>\" per item. xuid is decimal or 0x hex; source is 1 to 64 of A-Z a-z 0-9 . _ : -.",
		"An account keeps its items when it deletes its avatar.",
	}
	bearer := "Authorization: Bearer <token from POST /v1/auth/login>"
	authErr := contractError{401, "invalid_token, token_expired, ip_changed", "no live session for the token (auth.json)"}
	k.Endpoints = []contractEndpoint{
		{Method: "PUT", Path: "/v1/me/avatar", Auth: bearer,
			Request:  rawJSON(pyjson.Obj{{K: "manifest", V: "base64 of the 1000-byte manifest"}, {K: "colours", V: "the editor's colour list (optional)"}, {K: "body", V: "base64 of the full-body PNG"}}),
			Response: rawJSON(pyjson.Obj{{K: "version", V: "the avatar's new avatarVersion (float)"}}),
			Errors: []contractError{
				{413, "too_large", fmt.Sprintf("the request's Content-Length is over %d bytes: the HTTP front answers {\"error\": \"too_large\"} without reading it", avatarRequestMax)},
				authErr,
				{429, "rate_limited", fmt.Sprintf("over %d saves in 10 minutes by this account (refused requests count)", avatarSavesPer10m)},
				{400, "bad_request", "the body is not a JSON object"},
				{400, "bad_manifest", "manifest is missing, not a base64 string, or not 1000 bytes"},
				{400, "bad_colours", "colours is not a string or not a colour list (limits)"},
				{400, "bad_image", "body is missing or not base64, not a PNG, over 2 MB, under 64 or over 1024 pixels either way, or a PNG the server cannot read (16-bit, interlaced)"},
				{403, "item_not_owned", "the manifest wears an item of the reward catalog that the account does not own"}},
			Notes: []string{"Checked in the order of the errors above; the first that applies answers.", "A save replaces the whole avatar."}},
		{Method: "DELETE", Path: "/v1/me/avatar", Auth: bearer, Response: rawJSON(pyjson.Obj{{K: "ok", V: true}}),
			Errors: []contractError{authErr}, Notes: []string{"Also when there is no avatar. The card's avatarVersion is 0 afterwards; the owned items stay."}},
		{Method: "GET", Path: "/v1/me/avatar/items", Auth: bearer,
			Response: rawJSON(pyjson.Obj{{K: "items", V: []any{pyjson.Obj{{K: "item", V: "item id"}, {K: "grantedAt", V: "seconds since 1970 (float)"}, {K: "source", V: "what granted it"}}}}}),
			Errors:   []contractError{authErr}, Notes: []string{"The oldest grant first; [] when the account owns nothing."}},
		{Method: "GET", Path: "/v1/avatar/<xuid>", Auth: "none (public, like /v1/gamerpic/<xuid>.png)",
			Response: rawJSON(pyjson.Obj{{K: "manifest", V: "base64 of the 1000 bytes"}, {K: "colours", V: "the colour list as saved"}, {K: "version", V: "avatarVersion (float)"}}),
			Errors:   []contractError{{404, "no_avatar", "the player has no avatar (or no account)"}, {404, "not_found", "<xuid> is not a decimal account id"}},
			Notes:    []string{"<xuid> in decimal, as the card's xuid. A query such as ?v=<version> is ignored."}},
		{Method: "GET", Path: "/v1/avatar/<xuid>.png", Auth: "none (public)",
			Response: rawJSON("the body picture, Content-Type image/png, Cache-Control max-age=60: add ?v=<avatarVersion> to fetch a new save at once"),
			Errors:   []contractError{{404, "no_avatar", "the player has no avatar"}, {404, "not_found", "<xuid> is not a decimal account id"}}},
	}
	_, bodySha := pixelsOf(t, avatarBody64x128)
	k.Fixtures = []contractFixture{
		{"body64x128", "the body picture the run uploads: 64 x 128 RGBA, made with Go's image/png (any encoder will do)", base64.StdEncoding.EncodeToString(avatarBody64x128), bodySha},
		{"grey32", "a 32 x 32 grey PNG: too small", base64.StdEncoding.EncodeToString(avatarGrey32), ""},
		{"wide1025", "a PNG header of 1025 x 64 and nothing else: refused by its header", base64.StdEncoding.EncodeToString(headerOnlyPNG(1025, 64)), ""},
		{"manifest", "a made-up 1000-byte manifest (no real avatar's): ids in the stock form with invented numbers, nine colours, no reward item", base64.StdEncoding.EncodeToString(avatarManifest()), ""},
		{"manifestWithReward", "the same with the test reward item " + avatarReward.String() + " at worn slot 2 (0x1A0)", base64.StdEncoding.EncodeToString(avatarManifest(avatarReward)), ""},
	}
	k.Run.About = []string{
		"The steps run in order on one empty database, with these accounts signed in, and the reward catalog holding the items in rewards (the shipped catalog is empty).",
		"clock is the server's time from that step on, in seconds since 1970: versions and grant times come from it. A step with repeat is sent that many times, each answered alike.",
		"control is an operators' control line and answer its text. pick: the answer has more fields than these, and the contract pins only the ones listed. png: the picture's size and the SHA-256 of its decoded pixel rows (width x channels bytes each, top row first).",
	}
	k.Run.Accounts = []avatarAccount{avatarAnn, avatarBob, avatarCy}
	k.Run.Rewards = []string{avatarReward.String()}
	ann := fmt.Sprint(avatarAnn.Xuid)
	get := func(path, as string) *contractRequest { return &contractRequest{Method: "GET", Path: path, As: as} }
	put := func(as string, body json.RawMessage) *contractRequest {
		return &contractRequest{Method: "PUT", Path: "/v1/me/avatar", As: as, JSON: body}
	}
	card := []string{"player.xuid", "player.gamerpicVersion", "player.avatarVersion"}
	m, mr := avatarManifest(), avatarManifest(avatarReward)
	short := m[:999]
	k.Run.Steps = []contractStep{
		{Note: "no avatar yet", Request: get("/v1/avatar/"+ann, "")},
		{Note: "no avatar yet: no picture", Request: get("/v1/avatar/"+ann+".png", "")},
		{Note: "not an account id", Request: get("/v1/avatar/ann", "")},
		{Note: "the card says avatarVersion 0", Request: get("/v1/community/player/"+ann, "bob"), Response: &contractResponse{Pick: card}},
		{Note: "nothing owned yet", Request: get("/v1/me/avatar/items", "ann")},
		{Note: "a save needs a token", Request: put("", putBody(m, avatarColours, avatarBody64x128))},
		{Note: "a manifest of 999 bytes", Request: put("ann", putBody(short, avatarColours, avatarBody64x128))},
		{Note: "a manifest that is not base64", Request: put("ann", rawJSON(pyjson.Obj{{K: "manifest", V: "not base64!"}, {K: "colours", V: ""}, {K: "body", V: base64.StdEncoding.EncodeToString(avatarBody64x128)}}))},
		{Note: "a colour line without its newline", Request: put("ann", putBody(m, "12 3", avatarBody64x128))},
		{Note: "a body that is not a PNG", Request: put("ann", putBody(m, avatarColours, []byte("GIF89a not a png")))},
		{Note: "a body of 32 x 32 pixels", Request: put("ann", putBody(m, avatarColours, avatarGrey32))},
		{Note: "a body 1025 pixels wide", Request: put("ann", putBody(m, avatarColours, headerOnlyPNG(1025, 64)))},
		{Note: "wearing the reward item without owning it", Request: put("ann", putBody(mr, avatarColours, avatarBody64x128))},
		{Note: "the operators grant it (the battle pass will, through avatars.Store.Grant)", Clock: 1800000100,
			Control: "/admin/avatar-items?xuid=" + avatarAnn.XuidHex + "&grant=" + avatarReward.String() + "&source=test"},
		{Note: "now it is owned", Request: get("/v1/me/avatar/items", "ann")},
		{Note: "the same save passes", Clock: 1800000200, Request: put("ann", putBody(mr, avatarColours, avatarBody64x128))},
		{Note: "anyone reads it back", Request: get("/v1/avatar/"+ann, "")},
		{Note: "the picture: the server's encoding of the same pixels", Request: get("/v1/avatar/"+ann+".png?v=1800000200.0", "")},
		{Note: "the card carries the version", Request: get("/v1/community/player/"+ann, "bob"), Response: &contractResponse{Pick: card}},
		{Note: "a later save without the reward item, no colour choices", Clock: 1800000300, Request: put("ann", putBody(m, "", avatarBody64x128))},
		{Note: "the later save read back", Request: get("/v1/avatar/"+ann, "")},
		{Note: "a second grant changes nothing", Clock: 1800000400, Control: "/admin/avatar-items?xuid=" + avatarAnn.XuidHex + "&grant=" + strings.ToUpper(avatarReward.String())},
		{Note: "the control line's list", Control: "/admin/avatar-items?xuid=" + ann},
		{Note: "a grant to an xuid without an account", Control: "/admin/avatar-items?xuid=0x9000000000aff&grant=" + avatarReward.String() + "&source=test"},
		{Note: "the empty id cannot be granted", Control: "/admin/avatar-items?xuid=" + avatarAnn.XuidHex + "&grant=00000000000000000000000000000000"},
		{Note: "the avatar goes", Request: &contractRequest{Method: "DELETE", Path: "/v1/me/avatar", As: "ann"}},
		{Note: "no avatar any more", Request: get("/v1/avatar/"+ann, "")},
		{Note: "no picture any more", Request: get("/v1/avatar/"+ann+".png", "")},
		{Note: "the card is back at 0", Request: get("/v1/community/player/"+ann, "bob"), Response: &contractResponse{Pick: card}},
		{Note: "the items stay", Request: get("/v1/me/avatar/items", "ann")},
		{Note: "deleting again is no error", Request: &contractRequest{Method: "DELETE", Path: "/v1/me/avatar", As: "ann"}},
		{Note: fmt.Sprintf("%d saves in 10 minutes are taken, refused ones too", avatarSavesPer10m), Repeat: avatarSavesPer10m, Request: put("cy", rawJSON(pyjson.Obj{}))},
		{Note: "the next one is refused before it is read", Request: put("cy", putBody(m, avatarColours, avatarBody64x128))},
		{Note: "another account is not held back", Clock: 1800000500, Request: put("ann", putBody(m, avatarColours, avatarBody64x128))},
	}
	return k
}

// pick copies the listed dotted paths of an answer.
func pick(v any, paths []string) pyjson.Obj {
	out := pyjson.Obj{}
	for _, p := range paths {
		out = pickInto(out, v, strings.Split(p, "."))
	}
	return out
}

func pickInto(dst pyjson.Obj, src any, keys []string) pyjson.Obj {
	o, _ := src.(pyjson.Obj)
	val, _ := o.Get(keys[0])
	if len(keys) > 1 {
		sub, _ := dst.Get(keys[0])
		so, _ := sub.(pyjson.Obj)
		val = pickInto(so, val, keys[1:])
	}
	dst.Set(keys[0], val)
	return dst
}

// fill runs the draft's steps and records the server's answers.
func (k *avatarContract) fill(t *testing.T) {
	g := newAvatarRig(t, k.Run.Accounts)
	g.c.Avatars.Rewards = map[avatars.ItemID]bool{}
	for _, r := range k.Run.Rewards {
		g.c.Avatars.Rewards[mustItem(r)] = true
	}
	for i := range k.Run.Steps {
		s := &k.Run.Steps[i]
		if s.Clock != 0 {
			g.clock = s.Clock
		}
		if s.Control != "" {
			s.Answer = g.control(s.Control)
			continue
		}
		n := max(s.Repeat, 1)
		var resp contractResponse
		for j := 0; j < n; j++ {
			var pk []string
			if s.Response != nil {
				pk = s.Response.Pick
			}
			status, text, raw, ctype := g.do(s.Request.As, s.Request.Method, s.Request.Path, []byte(s.Request.JSON))
			r := contractResponse{Status: status, ContentType: ctype, Pick: pk}
			switch {
			case raw != nil:
				im, sha := pixelsOf(t, raw)
				r.PNG = &contractPNG{im.W, im.H, im.Ch, sha}
			case pk != nil:
				v, err := pyjson.Parse(text)
				if err != nil {
					t.Fatal(err)
				}
				r.JSON = rawJSON(pick(v, pk))
			default:
				r.JSON = json.RawMessage(text)
			}
			if j > 0 && !contractSame(r, resp) {
				t.Fatalf("step %d (%s): answer %d of %d differs", i, s.Note, j+1, n)
			}
			resp = r
		}
		s.Response = &resp
	}
}

func contractSame(a, b contractResponse) bool {
	x, _ := json.Marshal(a)
	y, _ := json.Marshal(b)
	return bytes.Equal(x, y)
}

func (k *avatarContract) encode() []byte {
	var b bytes.Buffer
	e := json.NewEncoder(&b)
	e.SetEscapeHTML(false)
	e.SetIndent("", "  ")
	if err := e.Encode(k); err != nil {
		panic(err)
	}
	return b.Bytes()
}

// The contract file is what the server answers now, byte for byte: every endpoint, error, limit and the item id form Encore builds on.
// FH2MP_WRITE_AVATAR_CONTRACT=1 writes it (then look at the diff).
func TestAvatarContract(t *testing.T) {
	k := avatarContractDraft(t)
	k.fill(t)
	got := k.encode()
	if os.Getenv("FH2MP_WRITE_AVATAR_CONTRACT") == "1" {
		if err := os.WriteFile(avatarContractPath, got, 0o644); err != nil {
			t.Fatal(err)
		}
		t.Logf("wrote %s (%d bytes)", avatarContractPath, len(got))
		return
	}
	want, err := os.ReadFile(avatarContractPath)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		gl, wl := strings.Split(string(got), "\n"), strings.Split(string(want), "\n")
		for i := 0; i < len(gl) && i < len(wl); i++ {
			if gl[i] != wl[i] {
				t.Fatalf("%s differs from the server's answers at line %d:\n file:   %s\n server: %s\n(FH2MP_WRITE_AVATAR_CONTRACT=1 rewrites it; Encore then copies it and raises its pin)",
					avatarContractPath, i+1, cut(wl[i], 200), cut(gl[i], 200))
			}
		}
		t.Fatalf("%s has %d lines, the server's answers %d", avatarContractPath, len(wl), len(gl))
	}
	// the run says what it is meant to: the round trip holds and the refusals are the ones named
	steps := k.Run.Steps
	codes := map[string]bool{}
	for _, s := range steps {
		if s.Response != nil && s.Response.JSON != nil {
			codes[errorOf(string(s.Response.JSON))] = true
		}
	}
	for _, c := range []string{"no_avatar", "not_found", "invalid_token", "bad_manifest", "bad_colours", "bad_image", "item_not_owned", "rate_limited"} {
		if !codes[c] {
			t.Errorf("no step of the run is answered %s", c)
		}
	}
	_, sha := pixelsOf(t, avatarBody64x128)
	served := 0
	for _, s := range steps {
		if s.Response != nil && s.Response.PNG != nil {
			served++
			if s.Response.PNG.PixelsSha256 != sha {
				t.Errorf("the served picture is not the uploaded one's pixels")
			}
		}
	}
	if served != 1 {
		t.Errorf("%d pictures served, 1 expected", served)
	}
}

// ---------------------------------------------------------------- each endpoint

func TestAvatarManifestMustBe1000Bytes(t *testing.T) {
	g := newAvatarRig(t, []avatarAccount{avatarAnn})
	for _, n := range []int{0, 1, 999, 1001, 2000} {
		if status, text := g.put("ann", make([]byte, n), avatarColours, avatarBody64x128); status != 400 || errorOf(text) != "bad_manifest" {
			t.Errorf("%d bytes: %d %s", n, status, text)
		}
	}
	if status, text := g.put("ann", nil, avatarColours, avatarBody64x128); status != 400 || errorOf(text) != "bad_manifest" {
		t.Errorf("no manifest: %d %s", status, text)
	}
	s, _ := pyjson.Dumps(pyjson.Obj{{K: "manifest", V: int64(1000)}, {K: "body", V: base64.StdEncoding.EncodeToString(avatarBody64x128)}}, pyjson.Opts{})
	if status, text, _, _ := g.do("ann", "PUT", "/v1/me/avatar", []byte(s)); status != 400 || errorOf(text) != "bad_manifest" {
		t.Errorf("a number for a manifest: %d %s", status, text)
	}
	if status, text := g.put("ann", make([]byte, 1000), avatarColours, avatarBody64x128); status != 200 {
		t.Fatalf("1000 zero bytes: %d %s", status, text)
	}
	a, err := g.c.Avatars.Get(avatarAnn.Xuid)
	if err != nil || a == nil || len(a.Manifest) != 1000 || a.Colours != avatarColours {
		t.Fatalf("stored %+v %v", a, err)
	}
}

// An item of the reward catalog is refused until the account owns it, wherever the manifest wears it; other items are free.
func TestAvatarUnownedRewardIsRefusedUntilGranted(t *testing.T) {
	g := newAvatarRig(t, []avatarAccount{avatarAnn, avatarBob})
	hat, shirt, free := mustItem("f0000000000000000000000000000002"), mustItem("f0000000000000000000000000000003"), mustItem("e0000000000000000000000000000004")
	g.c.Avatars.Rewards = map[avatars.ItemID]bool{hat: true, shirt: true}
	m := avatarManifest(free, hat)
	copy(m[avatars.AssetAt+avatars.AssetStep*20:], shirt[:]) // in the basic outfit, which the console puts back on
	if status, text := g.put("ann", m, avatarColours, avatarBody64x128); status != 403 || errorOf(text) != "item_not_owned" {
		t.Fatalf("two unowned reward items: %d %s", status, text)
	}
	if out := g.control("/admin/avatar-items?xuid=" + avatarAnn.XuidHex + "&grant=" + hat.String() + "&source=battlepass:s1:tier4"); !strings.HasPrefix(out, "granted ") {
		t.Fatalf("grant: %q", out)
	}
	if status, text := g.put("ann", m, avatarColours, avatarBody64x128); status != 403 || errorOf(text) != "item_not_owned" {
		t.Fatalf("one of two owned: %d %s", status, text)
	}
	if _, err := g.c.Avatars.Grant(avatarAnn.Xuid, shirt, "battlepass:s1:tier9"); err != nil {
		t.Fatal(err)
	}
	if status, text := g.put("ann", m, avatarColours, avatarBody64x128); status != 200 {
		t.Fatalf("both owned: %d %s", status, text)
	}
	// owning is per account
	if status, text := g.put("bob", m, avatarColours, avatarBody64x128); status != 403 || errorOf(text) != "item_not_owned" {
		t.Fatalf("Bob wearing Ann's items: %d %s", status, text)
	}
	// a face slot is checked as well
	f := avatarManifest()
	copy(f[avatars.FaceAt+avatars.FaceStep*14:], hat[:])
	if status, text := g.put("bob", f, avatarColours, avatarBody64x128); status != 403 {
		t.Fatalf("a reward id in a face slot: %d %s", status, text)
	}
	// with the catalog of this build (empty) everything is free
	g.c.Avatars.Rewards = map[avatars.ItemID]bool{}
	for _, id := range avatars.Rewards {
		g.c.Avatars.Rewards[id] = true
	}
	if status, text := g.put("bob", m, avatarColours, avatarBody64x128); status != 200 && len(avatars.Rewards) == 0 {
		t.Fatalf("an empty catalog refused: %d %s", status, text)
	}
	_, items, _, _ := g.do("ann", "GET", "/v1/me/avatar/items", nil)
	if !strings.Contains(items, hat.String()) || !strings.Contains(items, shirt.String()) || !strings.Contains(items, "battlepass:s1:tier9") {
		t.Fatalf("Ann's items: %s", items)
	}
}

func pngOf(t *testing.T, img image.Image) []byte {
	t.Helper()
	var b bytes.Buffer
	if err := png.Encode(&b, img); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func TestAvatarBodyPixelBounds(t *testing.T) {
	g := newAvatarRig(t, []avatarAccount{avatarAnn})
	m := avatarManifest()
	rgba := func(w, h int) []byte {
		im := image.NewNRGBA(image.Rect(0, 0, w, h))
		im.SetNRGBA(w/2, h/2, color.NRGBA{200, 10, 10, 255})
		return pngOf(t, im)
	}
	const between = "the picture must be between 64 and 1024 pixels each way"
	for _, c := range []struct {
		name    string
		body    []byte
		status  int
		message string
	}{
		{"64 x 64", rgba(64, 64), 200, ""},
		{"1024 x 1024", rgba(1024, 1024), 200, ""},
		{"64 x 1024 grey", pngOf(t, image.NewGray(image.Rect(0, 0, 64, 1024))), 200, ""},
		{"63 x 64", rgba(63, 64), 400, between},
		{"64 x 63", rgba(64, 63), 400, between},
		{"1025 x 64", rgba(1025, 64), 400, between},
		{"64 x 1025", headerOnlyPNG(64, 1025), 400, between},
		{"16-bit", pngOf(t, image.NewNRGBA64(image.Rect(0, 0, 64, 64))), 400, "that PNG cannot be read (8-bit, not interlaced, please)"},
		{"no IHDR", []byte("\x89PNG\r\n\x1a\n\x00\x00\x00\x00IEND\xaeB`\x82"), 400, "that PNG cannot be read"},
		{"a JPEG", []byte{0xff, 0xd8, 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F'}, 400, "send a PNG of at most 2 MB"},
		{"2 MB and one byte", append(rgba(64, 64), make([]byte, avatarBodyMaxBytes)...)[:avatarBodyMaxBytes+1], 400, "send a PNG of at most 2 MB"},
		{"exactly 2 MB", append(rgba(64, 64), make([]byte, avatarBodyMaxBytes)...)[:avatarBodyMaxBytes], 200, ""},
	} {
		status, text := g.put("ann", m, avatarColours, c.body)
		if status != c.status || (c.message != "" && !strings.Contains(text, `"message": "`+c.message+`"`)) {
			t.Errorf("%s: %d %s, want %d %q", c.name, status, cut(text, 200), c.status, c.message)
		}
		if c.status == 200 {
			_, _, served, ctype := g.do("", "GET", fmt.Sprintf("/v1/avatar/%d.png", avatarAnn.Xuid), nil)
			got, gotSha := pixelsOf(t, served)
			want, wantSha := pixelsOf(t, c.body)
			if ctype != "image/png" || got.W != want.W || got.H != want.H || gotSha != wantSha {
				t.Errorf("%s: served %dx%d %s %s, uploaded %dx%d", c.name, got.W, got.H, ctype, gotSha[:8], want.W, want.H)
			}
		}
	}
	if status, text := g.put("ann", m, avatarColours, nil); status != 400 || !strings.Contains(text, "the body picture must be a PNG, in base64") {
		t.Errorf("no body: %d %s", status, text)
	}
	// the whole request has its own limit, which the HTTP front applies before reading
	if n := g.l.BodyLimit("/v1/me/avatar"); n != avatarRequestMax || n < int64(4*((avatarBodyMaxBytes+2)/3)+4*((avatars.ManifestSize+2)/3)+2*avatars.ColoursMax+100) {
		t.Errorf("request limit %d does not hold a 2 MB picture in base64 with the rest", n)
	}
	if status, text, _, _ := g.do("ann", "PUT", "/v1/me/avatar", make([]byte, avatarRequestMax+1)); status != 413 || errorOf(text) != "too_large" {
		t.Errorf("an oversized request: %d %s", status, text)
	}
}

func TestAvatarRoundTripAndDelete(t *testing.T) {
	g := newAvatarRig(t, []avatarAccount{avatarAnn})
	ann := fmt.Sprint(avatarAnn.Xuid)
	m := avatarManifest()
	g.clock = 1800000500.25
	status, text := g.put("ann", m, avatarColours, avatarBody64x128)
	if status != 200 || jsonNum(t, text, "version") != 1800000500.25 {
		t.Fatalf("save: %d %s", status, text)
	}
	status, text, _, _ = g.do("", "GET", "/v1/avatar/"+ann, nil)
	v, _ := pyjson.Parse(text)
	o, _ := v.(pyjson.Obj)
	mb, _ := o.Get("manifest")
	cv, _ := o.Get("colours")
	if status != 200 || mb != base64.StdEncoding.EncodeToString(m) || cv != avatarColours || jsonNum(t, text, "version") != 1800000500.25 {
		t.Fatalf("read back: %d %s", status, cut(text, 200))
	}
	status, _, raw, ctype := g.do("", "GET", "/v1/avatar/"+ann+".png", nil)
	_, sha := pixelsOf(t, raw)
	_, want := pixelsOf(t, avatarBody64x128)
	if status != 200 || ctype != "image/png" || sha != want {
		t.Fatalf("picture: %d %s", status, ctype)
	}
	if status, text, _, _ = g.do("ann", "DELETE", "/v1/me/avatar", nil); status != 200 || text != `{"ok": true}` {
		t.Fatalf("delete: %d %s", status, text)
	}
	for _, p := range []string{"/v1/avatar/" + ann, "/v1/avatar/" + ann + ".png"} {
		if status, text, _, _ = g.do("", "GET", p, nil); status != 404 || errorOf(text) != "no_avatar" {
			t.Errorf("%s after delete: %d %s", p, status, text)
		}
	}
	for _, p := range []string{"/v1/avatar/-5", "/v1/avatar/0", "/v1/avatar/0x9000000000a01", "/v1/avatar/99999999999999999999", "/v1/avatar/" + ann + ".jpg", "/v1/avatar/"} {
		if status, text, _, _ = g.do("", "GET", p, nil); status != 404 || errorOf(text) != "not_found" {
			t.Errorf("%s: %d %s", p, status, text)
		}
	}
	for _, c := range []struct{ method, path string }{{"PUT", "/v1/me/avatar"}, {"DELETE", "/v1/me/avatar"}, {"GET", "/v1/me/avatar/items"}} {
		if status, text, _, _ = g.do("", c.method, c.path, nil); status != 401 {
			t.Errorf("%s %s without a token: %d %s", c.method, c.path, status, text)
		}
	}
}

// avatarVersion on every card: 0 without an avatar, the save's version after one, raised by every save, 0 again after a delete.
func TestAvatarVersionOnTheCard(t *testing.T) {
	g := newAvatarRig(t, []avatarAccount{avatarAnn, avatarBob})
	if err := g.c.DB.Do(func(tx *db.Tx) error {
		_, err := tx.Exec("INSERT INTO friendships (a, b, created) VALUES (?,?,?)", avatarAnn.Xuid, avatarBob.Xuid, 1.0)
		return err
	}); err != nil {
		t.Fatal(err)
	}
	cardOf := func() (player, friend float64) {
		t.Helper()
		_, text, _, _ := g.do("bob", "GET", fmt.Sprintf("/v1/community/player/%d", avatarAnn.Xuid), nil)
		player = jsonNum(t, text, "player", "avatarVersion")
		_, text, _, _ = g.do("bob", "GET", "/v1/community", nil)
		v, _ := pyjson.Parse(text)
		o, _ := v.(pyjson.Obj)
		fl, _ := o.Get("friends")
		list, _ := fl.([]any)
		if len(list) != 1 {
			t.Fatalf("Bob's friends: %s", cut(text, 300))
		}
		s, _ := pyjson.Dumps(list[0], pyjson.Opts{})
		return player, jsonNum(t, s, "avatarVersion")
	}
	if p, f := cardOf(); p != 0 || f != 0 {
		t.Fatalf("before a save: %v %v", p, f)
	}
	_, text := g.put("ann", avatarManifest(), avatarColours, avatarBody64x128)
	v1 := jsonNum(t, text, "version")
	if p, f := cardOf(); p != v1 || f != v1 || v1 != 1800000000 {
		t.Fatalf("after a save at version %v: %v %v", v1, p, f)
	}
	_, text = g.put("ann", avatarManifest(), "", avatarBody64x128) // same clock
	v2 := jsonNum(t, text, "version")
	if p, _ := cardOf(); v2 <= v1 || p != v2 {
		t.Fatalf("a second save at the same time: %v after %v, card %v", v2, v1, p)
	}
	g.do("ann", "DELETE", "/v1/me/avatar", nil)
	if p, f := cardOf(); p != 0 || f != 0 {
		t.Fatalf("after the delete: %v %v", p, f)
	}
}

func TestAvatarItemsControlLine(t *testing.T) {
	g := newAvatarRig(t, []avatarAccount{avatarAnn})
	x := avatarAnn.XuidHex
	for line, want := range map[string]string{
		"/admin/avatar-items?xuid=" + x:                                                  "0x9000000000a01 owns no avatar items\n",
		"/admin/avatar-items?xuid=nobody&grant=" + avatarReward.String():                 "refused: xuid 'nobody' is not an account id (decimal, or hex with 0x)\n",
		"/admin/avatar-items?xuid=" + x + "&grant=f00":                                   "refused: an item id is 32 hex digits, this has 3 characters\n",
		"/admin/avatar-items?xuid=" + x + "&grant=" + avatarReward.String() + "&source=": "refused: the source \"\" is not 1 to 64 of A-Z a-z 0-9 . _ : -\n",
		"/admin/avatar-items?xuid=0x9000000000aff&grant=" + avatarReward.String():        "refused: no account has the xuid 0x9000000000aff\n",
	} {
		if got := g.control(line); got != want {
			t.Errorf("%s: %q, want %q", line, got, want)
		}
	}
	g.clock = 1800000123.5
	if got := g.control("/admin/avatar-items?xuid=" + fmt.Sprint(avatarAnn.Xuid) + "&grant=" + avatarReward.String()); got != "granted f0000000000000000000000000000001 to 0x9000000000a01 (source admin)\n" {
		t.Errorf("grant: %q", got)
	}
	if got := g.control("/admin/avatar-items?xuid=" + x); got != "f0000000000000000000000000000001 granted 1800000123.5 by admin\n" {
		t.Errorf("list: %q", got)
	}
	// no client request grants
	for _, c := range []struct{ method, path string }{{"POST", "/v1/me/avatar/items"}, {"PUT", "/v1/me/avatar/items"}, {"POST", "/v1/me/avatar"}} {
		if status, text, _, _ := g.do("ann", c.method, c.path, []byte(`{"item": "f0000000000000000000000000000001"}`)); status != 404 {
			t.Errorf("%s %s: %d %s", c.method, c.path, status, text)
		}
	}
}

func TestAvatarSavesAreRateLimited(t *testing.T) {
	g := newAvatarRig(t, []avatarAccount{avatarAnn})
	for i := 0; i < avatarSavesPer10m; i++ {
		if status, _ := g.put("ann", avatarManifest(), avatarColours, avatarBody64x128); status != 200 {
			t.Fatalf("save %d: %d", i+1, status)
		}
	}
	status, text := g.put("ann", avatarManifest(), avatarColours, avatarBody64x128)
	if status != 429 || text != `{"error": "rate_limited", "message": "too many avatar saves, wait a few minutes"}` {
		t.Fatalf("save %d: %d %s", avatarSavesPer10m+1, status, text)
	}
}

// The gamerpic check shares cleanPNG with the body picture and keeps its own limits: 32 to 512 pixels each way, 1 MB.
func TestGamerpicKeepsItsOwnLimits(t *testing.T) {
	sq := func(w, h int) []byte { return pngOf(t, image.NewNRGBA(image.Rect(0, 0, w, h))) }
	for _, c := range []struct {
		name, message string
		png           []byte
	}{
		{"32 x 32", "", sq(32, 32)},
		{"512 x 512", "", sq(512, 512)},
		{"31 x 32", "the picture must be between 32 and 512 pixels each way", sq(31, 32)},
		{"513 x 64", "the picture must be between 32 and 512 pixels each way", sq(513, 64)},
		{"64 x 128 avatar body", "", avatarBody64x128},
		{"1 MB and one byte", "send a PNG of at most 1 MB", append(sq(32, 32), make([]byte, 1_000_000)...)[:1_000_001]},
	} {
		_, err := cleanGamerpic(c.png)
		got := ""
		if err != nil {
			got = err.Error()
		}
		if got != c.message {
			t.Errorf("%s: %q, want %q", c.name, got, c.message)
		}
	}
}
