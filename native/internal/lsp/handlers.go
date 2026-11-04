package lsp

import (
	"fmt"
	"strings"

	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/xls"
)

// handlerFn answers one Xls call: the decoded request (nil when there is none) and the result struct, or nil for the call's all-default result.
type handlerFn func(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error)

// rawFn answers a call the exe has no descriptor for: the request body in, the response payload out.
type rawFn func(c *Core, r *Req, body []byte) []byte

var (
	handlers    = map[[2]string]handlerFn{}
	rawHandlers = map[[2]string]rawFn{}
)

func register(class, method string, fn handlerFn) { handlers[[2]string{class, method}] = fn }

// failUntilImplemented are reads whose "empty success" would be cached by the client and replace its built-in data. Until real data is served they
// fail, so the client keeps using what it ships with.
var failUntilImplemented = map[[2]string]bool{
	{"Forza.WebServices.Drivatar", "GetDrivatarInformationForTrack"}: true,
	// Sending a car deletes it locally; an error makes the game put it back, a default OK loses it for good.
	{"Forza.WebServices.Gifting", "SendCarGift"}:           true,
	{"Forza.WebServices.Auction", "UploadAuctionTextures"}: true,
	{"Forza.WebServices.GiftLivery", "Create"}:             true,
	{"Forza.WebServices.GiftTuning", "Create"}:             true,
	{"Forza.WebServices.GiftLayerGroup", "Create"}:         true,
}

var ghostUpload = [2]string{"Forza.WebServices.Scoreboard", "UploadFile"}

// Rate limits: SubmitScore per account (count, window s); UploadFile (the busiest day of 17,330 runs was 64).
var (
	scoreLimit  = [][2]int{{120, 60}}
	ghostLimits = [][2]int{{10, 60}, {300, 86400}}
)

// within says whether the requesting account is inside every one of the limits (and counts the call).
func (c *Core) within(r *Req, name string, limits [][2]int) bool {
	if c.Auth == nil {
		return true
	}
	ok := true
	for _, l := range limits {
		if !c.Auth.Hit([3]any{name, r.Xuid, l[1]}, l[0], float64(l[1])) {
			ok = false
			break
		}
	}
	return ok
}

// callName is `Notifications.RetrieveForUser`, or the host for an Xbox Live call: what a request is for, from its headers.
func callName(class, method, host string) string {
	cls, _, _ := strings.Cut(class, ",")
	cls = strings.TrimSpace(cls)
	if i := strings.LastIndex(cls, "."); i >= 0 {
		cls = cls[i+1:]
	}
	if cls != "" && method != "" {
		return cls + "." + method
	}
	if method != "" {
		return method
	}
	if host != "" {
		return host
	}
	return "?"
}

// handleXls runs one Xls call and returns the framed response.
func (c *Core) handleXls(r *Req, cls, method string, body []byte) []byte {
	base, _, _ := strings.Cut(cls, ",")
	key := [2]string{strings.TrimSpace(base), method}
	call := callName(key[0], method, "")
	if fn, ok := rawHandlers[key]; ok {
		return frame(fn(c, r, body), 0, 0, nil)
	}
	_, have := handlers[key]
	if failUntilImplemented[key] && !have {
		c.Log(r, "  not implemented -> error result (client keeps its local data) call="+call)
		return frame(nil, 2, 1, []byte("fh2mp: not implemented"))
	}
	pname, pfields := c.findStruct(cls, method, "_Parameters")
	var req *xls.Struct
	limited := key == ghostUpload && !c.within(r, "ghost", ghostLimits)
	switch {
	case limited:
		var parts []string
		for _, l := range ghostLimits {
			parts = append(parts, fmt.Sprintf("%d per %d s", l[0], l[1]))
		}
		c.LogAt(r, diag.Warn, "", fmt.Sprintf("  UploadFile over the rate limit, ignored (user=%#x, %d bytes; limits %s)", r.Xuid, len(body), strings.Join(parts, " and ")))
	case pfields != nil && len(body) > 0 && have:
		dec, rem, err := c.SC.Decode(pfields, body)
		if err != nil {
			c.LogAt(r, diag.Warn, "", fmt.Sprintf("  request decode failed: %v (call=%s, body %d bytes, schema %s); the handler gets no request", err, call, len(body), pname))
		} else {
			req = dec
			if rem > 0 {
				c.LogAt(r, diag.Warn, "", fmt.Sprintf("  warning: %d unread request bytes (call=%s, body %d bytes, schema %s)", rem, call, len(body), pname))
			}
		}
	case pfields != nil && len(body) == 0:
		c.LogAt(r, diag.Debug, "xls", fmt.Sprintf("  request schema %s (empty body)", pname))
	}
	rname, rfields := c.findStruct(cls, method, "_Results")
	if rfields == nil && !have {
		c.LogEvery(r, fmt.Sprintf("no-schema/%v", key), 600, fmt.Sprintf("  no schema for %s %s; answering OK with empty body (a call this server does not know)", cls, method), diag.Warn, "")
		return frame(nil, 0, 0, nil)
	}
	var result *xls.Struct
	if have && !limited {
		var err error
		result, err = c.runHandler(handlers[key], r, req)
		if err != nil {
			if xe, ok := err.(*XlsError); ok {
				c.Log(r, fmt.Sprintf("  -> error result: %s (error %#x, call=%s)", xe.Text, xe.Code, call))
				return frame(nil, 2, uint32(xe.Code), []byte(xe.Text))
			}
			panic(err) // the HTTP front turns it into an error result and logs it
		}
	}
	if result == nil {
		if rfields == nil {
			return frame(nil, 0, 0, nil)
		}
		result = c.def(rfields)
		if !have {
			c.LogEvery(r, fmt.Sprintf("default-result/%v", key), 600, fmt.Sprintf("  default %s (no handler for %s: answered with an all-default result)", rname, call), diag.Info, "")
		}
	}
	payload, err := c.SC.Encode(rfields, result)
	if err != nil {
		panic(err)
	}
	return frame(payload, 0, 0, nil)
}

// runHandler runs a handler; a *XlsError it panics with (from helpers that cannot return one) is an error result like a returned one.
func (c *Core) runHandler(fn handlerFn, r *Req, req *xls.Struct) (res *xls.Struct, err error) {
	defer func() {
		if p := recover(); p != nil {
			if xe, ok := p.(*XlsError); ok {
				res, err = nil, xe
				return
			}
			panic(p)
		}
	}()
	return fn(c, r, req)
}

// ---------------------------------------------------------------- JSON <-> struct

// structToJSON turns a decoded struct tree into an ordered JSON tree (what the stores keep).
func structToJSON(v any) any {
	switch x := v.(type) {
	case *xls.Struct:
		if x == nil {
			return nil
		}
		o := make(pyjson.Obj, 0, len(x.Names))
		for _, k := range x.Names {
			o = append(o, pyjson.Pair{K: k, V: structToJSON(x.Vals[k])})
		}
		return o
	case []any:
		out := make([]any, len(x))
		for i, e := range x {
			out[i] = structToJSON(e)
		}
		return out
	case int:
		return int64(x)
	}
	return v
}

// jsonToStruct turns an ordered JSON tree (a stored record) into a struct tree the encoder takes.
func jsonToStruct(v any) any {
	switch x := v.(type) {
	case pyjson.Obj:
		s := xls.NewStruct()
		for _, p := range x {
			s.Set(p.K, jsonToStruct(p.V))
		}
		return s
	case []any:
		out := make([]any, len(x))
		for i, e := range x {
			out[i] = jsonToStruct(e)
		}
		return out
	}
	return v
}

func list(vals ...any) []any { return vals }
