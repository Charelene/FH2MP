package lsp

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"fh2mp/internal/flags"
)

func opsErrorOf(f func()) (e *OpsError) {
	defer func() {
		if r := recover(); r != nil {
			e, _ = r.(*OpsError)
			if e == nil {
				panic(r)
			}
		}
	}()
	f()
	return nil
}

// PUT /ops/v1/config {"features": {...}} writes the realm's features file (the game server reads it too) and nothing else; the status answer
// and GET /ops/v1/config show the switches in force.
func TestOpsWritesTheFeatureSwitchesToTheFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "features.json")
	set := flags.New(path)
	defer flags.SetDefault(set)()
	c := New()
	c.Out = func(string) {}
	o := &Ops{C: c}

	body := opsJSON([]byte(`{"features": {"voice": false}, "motd": "hi"}`))
	rest, changed := o.takeFeatures(&Req{}, body)
	if !changed || len(rest) != 1 || rest[0].K != "motd" {
		t.Fatalf("rest = %v, changed = %v, want only motd left", rest, changed)
	}
	if set.On(flags.Voice) || !set.On(flags.Lobbies) {
		t.Fatalf("switches %v, want only voice off", set.Snapshot())
	}
	raw, _ := os.ReadFile(path)
	if !strings.Contains(string(raw), `"voice": false`) || !strings.Contains(string(raw), `"lobbies": true`) {
		t.Fatalf("file %q", raw)
	}
	obj := featuresObj()
	if v, _ := obj.Get("voice"); v != false {
		t.Fatalf("featuresObj voice = %v", v)
	}
	if v, _ := obj.Get("lobbies"); v != true {
		t.Fatalf("featuresObj lobbies = %v", v)
	}
	if rest, changed := o.takeFeatures(&Req{}, opsJSON([]byte(`{"motd": "x"}`))); changed || len(rest) != 1 {
		t.Fatal("a body without features must pass through")
	}
	for _, bad := range []string{`{"features": {"voic": false}}`, `{"features": {"voice": "off"}}`, `{"features": {}}`, `{"features": [1]}`} {
		if e := opsErrorOf(func() { o.takeFeatures(&Req{}, opsJSON([]byte(bad))) }); e == nil || e.Status != 400 {
			t.Fatalf("%s: got %v, want 400", bad, e)
		}
	}
}

func TestOpsFeaturesNeedAFile(t *testing.T) {
	defer flags.SetDefault(flags.New(""))()
	o := &Ops{C: New()}
	o.C.Out = func(string) {}
	if e := opsErrorOf(func() { o.takeFeatures(&Req{}, opsJSON([]byte(`{"features": {"voice": false}}`))) }); e == nil || e.Status != 409 {
		t.Fatalf("got %v, want 409 with no features file", e)
	}
}
