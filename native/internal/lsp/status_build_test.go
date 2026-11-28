package lsp

import (
	"net/http"
	"strings"
	"testing"

	"fh2mp/internal/auth"
	"fh2mp/internal/pyjson"
)

// /v1/status says which commit the server runs: a "build" object with commit, time, modified and goVersion, after the keys the launcher
// has always read (a test binary has no commit stamp, so only the Go version has a value here). The route needs no database.
func TestStatusAnswerCarriesTheBuildStamp(t *testing.T) {
	c := New()
	c.Auth = &auth.Service{Env: "dev", Mode: "optional", Registration: "open"}
	l := NewLauncher(c)
	status, obj, _ := l.Handle(&Req{}, "GET", "/v1/status", http.Header{}, nil, "127.0.0.1")
	if status != 200 {
		t.Fatalf("status %d: %v", status, obj)
	}
	o, ok := obj.(pyjson.Obj)
	if !ok {
		t.Fatalf("answer is %T", obj)
	}
	keys := make([]string, len(o))
	for i, p := range o {
		keys[i] = p.K
	}
	if got, want := strings.Join(keys, ","), "env,online,auth,registration,server,time,maintenance,build"; got != want {
		t.Fatalf("keys %s, want %s", got, want)
	}
	b, _ := o.Get("build")
	bo, ok := b.(pyjson.Obj)
	if !ok {
		t.Fatalf("build is %T", b)
	}
	keys = keys[:0]
	for _, p := range bo {
		keys = append(keys, p.K)
	}
	if got, want := strings.Join(keys, ","), "commit,time,modified,goVersion"; got != want {
		t.Fatalf("build keys %s, want %s", got, want)
	}
	if gv, _ := bo.Get("goVersion"); !strings.HasPrefix(pyjson.PyStr(gv), "go") {
		t.Fatalf("goVersion %v", gv)
	}
	if _, err := pyjson.Dumps(obj, pyjson.Opts{}); err != nil {
		t.Fatal(err)
	}
}
