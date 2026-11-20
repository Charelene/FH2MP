package lsp

import (
	"encoding/base64"
	"errors"
	"fmt"
	"strconv"
	"strings"

	"fh2mp/internal/auth"
	"fh2mp/internal/avatars"
	"fh2mp/internal/pyjson"
)

// A player's Xbox 360 avatar as Encore saves it, and the avatar items the account owns (internal/avatars). The contract, with every request,
// answer, refusal and limit, is native/testdata/avatar_api.json; Encore keeps a copy and checks it against this commit.
//
//	PUT    /v1/me/avatar          {"manifest": base64 of 1000 bytes, "colours": "<record> <palette>\n"..., "body": base64 of a PNG} -> {"version"}
//	DELETE /v1/me/avatar          -> {"ok": true}; the owned items stay
//	GET    /v1/me/avatar/items    -> {"items": [{"item", "grantedAt", "source"}]}
//	GET    /v1/avatar/<xuid>      -> {"manifest", "colours", "version"}   (public, like the gamerpic)
//	GET    /v1/avatar/<xuid>.png  -> the body picture                     (public)
//
// The community card carries avatarVersion (0 = no avatar). Items are granted by the server only (avatars.Store.Grant, the battle pass
// later, the operators' GET /admin/avatar-items line for tests); there is no client call that grants.

// The limits of PUT /v1/me/avatar: the body picture as sent and its pixels each way, the whole request (the picture in base64 with the
// manifest and the colour list), and the saves an account may make in 10 minutes.
const (
	avatarBodyMaxBytes = 2_000_000
	avatarBodyMinDim   = 64
	avatarBodyMaxDim   = 1024
	avatarRequestMax   = 2_800_000
	avatarSavesPer10m  = 20
)

// avatarGet answers GET /v1/avatar/<xuid> and /v1/avatar/<xuid>.png (xuid in decimal, as the card has it).
func (l *Launcher) avatarGet(name string) (int, any, string, error) {
	id, png := strings.CutSuffix(name, ".png")
	if !isDigits(id) {
		return 0, nil, "", auth.NewError(404, "not_found", "")
	}
	xuid, err := strconv.ParseInt(id, 10, 64)
	if err != nil || xuid <= 0 {
		return 0, nil, "", auth.NewError(404, "not_found", "")
	}
	noAvatar := auth.NewError(404, "no_avatar", "this player has no avatar").WithDetail(fmt.Sprintf("no avatar saved for %#x", xuid))
	if png {
		body, err := l.C.Avatars.Body(xuid)
		if err != nil {
			return 0, nil, "", err
		}
		if body == nil {
			return 0, nil, "", noAvatar
		}
		return 200, body, "image/png", nil
	}
	a, err := l.C.Avatars.Get(xuid)
	if err != nil {
		return 0, nil, "", err
	}
	if a == nil {
		return 0, nil, "", noAvatar
	}
	return 200, pyjson.Obj{{K: "manifest", V: base64.StdEncoding.EncodeToString(a.Manifest)}, {K: "colours", V: a.Colours}, {K: "version", V: a.Version}},
		"application/json", nil
}

// b64Field is a base64 string field of a request (standard alphabet with padding); why says what is wrong with it.
func b64Field(d pyjson.Obj, key string) (b []byte, why string) {
	v, has := d.Get(key)
	if !has || v == nil {
		return nil, key + " is missing"
	}
	s, isStr := v.(string)
	if !isStr {
		return nil, fmt.Sprintf("%s is a %s, not a string", key, pyjson.TypeName(v))
	}
	b, err := base64.StdEncoding.Strict().DecodeString(s)
	if err != nil {
		return nil, fmt.Sprintf("%s is not base64: %v", key, err)
	}
	return b, ""
}

// avatarPut is PUT /v1/me/avatar. Checked in this order: the size of the request (413), 20 saves per 10 minutes (429), JSON, the manifest,
// the colour list, the body picture (400), then that every reward item the manifest wears is owned (403).
func (l *Launcher) avatarPut(r *Req, acct *auth.Account, body []byte) (any, error) {
	c, a := l.C, l.C.Auth
	if len(body) > avatarRequestMax {
		return nil, auth.NewError(413, "too_large", "").WithXuid(acct.Xuid).WithDetail(fmt.Sprintf("avatar request %d bytes, limit %d", len(body), avatarRequestMax))
	}
	if !a.Hit([2]any{"avatar", acct.Xuid}, avatarSavesPer10m, 600) {
		return nil, auth.NewError(429, "rate_limited", "too many avatar saves, wait a few minutes").WithXuid(acct.Xuid).
			WithDetail(fmt.Sprintf("avatar saves: over %d per 10 minutes for this account", avatarSavesPer10m))
	}
	d, err := jsonBody(body, avatarRequestMax)
	if err != nil {
		return nil, err
	}
	manifest, why := b64Field(d, "manifest")
	if why == "" && len(manifest) != avatars.ManifestSize {
		why = fmt.Sprintf("manifest is %d bytes", len(manifest))
	}
	if why != "" {
		return nil, auth.NewError(400, "bad_manifest", fmt.Sprintf("the avatar manifest must be %d bytes, in base64", avatars.ManifestSize)).WithXuid(acct.Xuid).WithDetail(why)
	}
	colours := ""
	if v, has := d.Get("colours"); has && v != nil {
		s, isStr := v.(string)
		if !isStr {
			return nil, auth.NewError(400, "bad_colours", "the avatar's colour list cannot be read").WithXuid(acct.Xuid).WithDetail("colours is a " + pyjson.TypeName(v) + ", not a string")
		}
		colours = s
	}
	if err := avatars.CheckColours(colours); err != nil {
		return nil, auth.NewError(400, "bad_colours", "the avatar's colour list cannot be read").WithXuid(acct.Xuid).WithDetail("colours: " + err.Error())
	}
	raw, why := b64Field(d, "body")
	if why != "" {
		return nil, auth.NewError(400, "bad_image", "the body picture must be a PNG, in base64").WithXuid(acct.Xuid).WithDetail(why)
	}
	png, err := cleanPNG(raw, avatarBodyLimits)
	if err != nil {
		var ae *auth.Error
		if errors.As(err, &ae) {
			ae.WithXuid(acct.Xuid)
		}
		return nil, err
	}
	version, err := c.Avatars.Save(acct.Xuid, manifest, colours, png)
	var missing *avatars.NotOwnedError
	if errors.As(err, &missing) {
		return nil, auth.NewError(403, "item_not_owned", "this avatar wears a reward item this account does not own").WithXuid(acct.Xuid).WithDetail(missing.Error())
	}
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  v1 avatar of %#x saved: version %.3f, %d item ids worn, %d colour choices, body %dx%d in %d bytes (%d sent)", acct.Xuid, version,
		len(avatars.Worn(manifest)), strings.Count(colours, "\n"), be32(png[16:]), be32(png[20:]), len(png), len(raw)))
	return pyjson.Obj{{K: "version", V: version}}, nil
}

// avatarItemsLine is the operators' control line for avatar items (loopback only, like every /admin/ endpoint; for tests until the battle
// pass grants):
//
//	GET /admin/avatar-items?xuid=<xuid>&grant=<item>[&source=<source>]   grants the item (avatars.Store.Grant; source "admin" when not given)
//	GET /admin/avatar-items?xuid=<xuid>                                 lists the items the account owns
//
// xuid is decimal or 0x hex, item 32 hex digits. One text answer; a refusal is one line starting "refused:".
func (c *Core) avatarItemsLine(q map[string]string) (string, error) {
	xuid, err := parseIntBase0(q["xuid"])
	if err != nil || xuid <= 0 {
		return fmt.Sprintf("refused: xuid %s is not an account id (decimal, or hex with 0x)\n", pyjson.PyRepr(q["xuid"])), nil
	}
	if g, has := q["grant"]; has {
		item, err := avatars.ParseItem(strings.TrimSpace(g))
		if err != nil {
			return "refused: " + err.Error() + "\n", nil
		}
		source := "admin"
		if s, has := q["source"]; has {
			source = s
		}
		granted, err := c.Avatars.Grant(xuid, item, source)
		var refused *avatars.RefusedError
		switch {
		case errors.As(err, &refused):
			return "refused: " + refused.Reason + "\n", nil
		case err != nil:
			return "", err
		case !granted:
			return fmt.Sprintf("%#x already owns %s\n", xuid, item), nil
		}
		return fmt.Sprintf("granted %s to %#x (source %s)\n", item, xuid, source), nil
	}
	items, err := c.Avatars.Items(xuid)
	if err != nil {
		return "", err
	}
	if len(items) == 0 {
		return fmt.Sprintf("%#x owns no avatar items\n", xuid), nil
	}
	var b strings.Builder
	for _, it := range items {
		fmt.Fprintf(&b, "%s granted %s by %s\n", it.Item, pyjson.PyStr(it.GrantedAt), it.Source)
	}
	return b.String(), nil
}
