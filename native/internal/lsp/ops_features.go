package lsp

import (
	"fmt"
	"strings"

	"fh2mp/internal/flags"
	"fh2mp/internal/pyjson"
)

// featuresObj is the operator's kill switches as the status answer and GET /ops/v1/config give them: every feature, true when it is on.
func featuresObj() pyjson.Obj {
	snap := flags.Default().Snapshot()
	out := pyjson.Obj{}
	for _, k := range flags.Known {
		out.Set(k.Name, snap[k.Name])
	}
	return out
}

// takeFeatures cuts "features" out of a PUT /ops/v1/config body and applies it: {"voice": false} switches voice off in the realm's features file
// (internal/flags), which the game server reads too, so both servers follow within a second and nothing restarts. The switches are not kept in
// the ops settings of the database: the file is their only home. It returns the rest of the body and whether switches were written.
func (o *Ops) takeFeatures(r *Req, d pyjson.Obj) (rest pyjson.Obj, changed bool) {
	fv, has := d.Get("features")
	if !has {
		return d, false
	}
	rest = pyjson.Obj{}
	for _, p := range d {
		if p.K != "features" {
			rest = append(rest, p)
		}
	}
	fo, isObj := fv.(pyjson.Obj)
	if !isObj || len(fo) == 0 {
		panic(opsErr(400, "bad_request", "features must be an object like {\"voice\": false}"))
	}
	changes := map[string]bool{}
	var bad []string
	for _, p := range fo {
		b, isBool := p.V.(bool)
		known := false
		for _, k := range flags.Known {
			known = known || k.Name == p.K
		}
		if !known || !isBool {
			bad = append(bad, p.K)
			continue
		}
		changes[p.K] = b
	}
	if len(bad) > 0 {
		names := make([]string, len(flags.Known))
		for i, k := range flags.Known {
			names[i] = k.Name
		}
		panic(opsErr(400, "bad_request", fmt.Sprintf("features may only be %s, each true or false (not: %s)", strings.Join(names, ", "), strings.Join(bad, ", "))))
	}
	set := flags.Default()
	before := set.Snapshot()
	if err := set.Update(changes); err != nil {
		panic(opsErr(409, "no_features_file", err.Error()))
	}
	var moved []string
	for _, k := range flags.Known {
		if nv, ok := changes[k.Name]; ok && before[k.Name] != nv {
			word := "on"
			if !nv {
				word = "off"
			}
			moved = append(moved, k.Name+" "+word)
		}
	}
	o.C.Log(r, fmt.Sprintf("  ops: features changed: %s", strings.Join(append(moved, "(written to the features file)"), "; ")))
	return rest, true
}
