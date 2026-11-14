package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeFile(t *testing.T, path, text string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

type fixture struct {
	dir string
	pub ed25519.PublicKey
	prv ed25519.PrivateKey
}

// a release as gh would hand it over: manifest.json, its signature and assets.json
func newRelease(t *testing.T, edit func(m map[string]any), assets map[string]int64) fixture {
	t.Helper()
	pub, prv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	m := map[string]any{
		"schema": 1, "tag": "v1.4.1",
		"shim": map[string]any{"version": "1.4.1", "name": "fh2mp-shim-1.4.1.zip", "size": 1000},
		"launcher": map[string]any{"version": "1.4.1", "assets": map[string]any{
			"windows-x64-installer": map[string]any{"name": "setup.exe", "size": 10},
			"windows-x64-portable":  map[string]any{"name": "portable.zip", "size": 11},
			"linux-deb":             map[string]any{"name": "x.deb", "size": 12},
			"linux-appimage":        map[string]any{"name": "x.AppImage", "size": 13},
		}},
	}
	if edit != nil {
		edit(m)
	}
	raw, _ := json.Marshal(m)
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "manifest.json"), string(raw))
	writeFile(t, filepath.Join(dir, "manifest.json.sig"), base64.StdEncoding.EncodeToString(ed25519.Sign(prv, raw))+"\n")
	if assets == nil {
		assets = map[string]int64{"fh2mp-shim-1.4.1.zip": 1000, "setup.exe": 10, "portable.zip": 11, "x.deb": 12, "x.AppImage": 13, "manifest.json": 5}
	}
	var list []map[string]any
	for n, s := range assets {
		list = append(list, map[string]any{"name": n, "size": s})
	}
	a, _ := json.Marshal(map[string]any{"assets": list})
	writeFile(t, filepath.Join(dir, "assets.json"), string(a))
	return fixture{dir, pub, prv}
}

func TestManifest(t *testing.T) {
	f := newRelease(t, nil, nil)
	out, err := checkManifest(f.dir, "v1.4.1", "1.4.1", "1.4.1", f.pub)
	if err != nil || out != "manifest ok: linux-appimage, linux-deb, windows-x64-installer, windows-x64-portable" {
		t.Fatalf("good release: %q %v", out, err)
	}
	other, _, _ := ed25519.GenerateKey(rand.Reader)
	if _, err := checkManifest(f.dir, "v1.4.1", "1.4.1", "1.4.1", other); err == nil || err.Error() != "the release manifest signature is not valid" {
		t.Fatalf("another key: %v", err)
	}
	if _, err := checkManifest(f.dir, "v1.4.2", "1.4.1", "1.4.1", f.pub); err == nil || err.Error() != "manifest tag mismatch" {
		t.Fatalf("tag: %v", err)
	}
	if _, err := checkManifest(f.dir, "v1.4.1", "1.4.0", "1.4.1", f.pub); err == nil || err.Error() != "manifest shim version '1.4.1', expected 1.4.0" {
		t.Fatalf("shim version: %v", err)
	}
	if _, err := checkManifest(f.dir, "v1.4.1", "1.4.1", "1.4.0", f.pub); err == nil || !strings.HasPrefix(err.Error(), "manifest launcher version") {
		t.Fatalf("launcher version: %v", err)
	}
	// a changed byte after signing
	raw, _ := os.ReadFile(filepath.Join(f.dir, "manifest.json"))
	writeFile(t, filepath.Join(f.dir, "manifest.json"), strings.Replace(string(raw), `"size":1000`, `"size":1001`, 1))
	if _, err := checkManifest(f.dir, "v1.4.1", "1.4.1", "1.4.1", f.pub); err == nil || err.Error() != "the release manifest signature is not valid" {
		t.Fatalf("tampered: %v", err)
	}

	short := newRelease(t, nil, map[string]int64{"fh2mp-shim-1.4.1.zip": 1000, "setup.exe": 10, "portable.zip": 3, "x.deb": 12, "x.AppImage": 13})
	if _, err := checkManifest(short.dir, "v1.4.1", "1.4.1", "1.4.1", short.pub); err == nil || err.Error() != "missing/incomplete release asset: portable.zip" {
		t.Fatalf("short asset: %v", err)
	}
	gone := newRelease(t, nil, map[string]int64{"setup.exe": 10, "portable.zip": 11, "x.deb": 12, "x.AppImage": 13})
	if _, err := checkManifest(gone.dir, "v1.4.1", "1.4.1", "1.4.1", gone.pub); err == nil || err.Error() != "missing/incomplete release asset: fh2mp-shim-1.4.1.zip" {
		t.Fatalf("missing shim zip: %v", err)
	}
	nodeb := newRelease(t, func(m map[string]any) {
		delete(m["launcher"].(map[string]any)["assets"].(map[string]any), "linux-deb")
	}, nil)
	if _, err := checkManifest(nodeb.dir, "v1.4.1", "1.4.1", "1.4.1", nodeb.pub); err == nil || err.Error() != "the manifest is missing launcher packages" {
		t.Fatalf("no deb: %v", err)
	}
	schema := newRelease(t, func(m map[string]any) { m["schema"] = 2 }, nil)
	if _, err := checkManifest(schema.dir, "v1.4.1", "1.4.1", "1.4.1", schema.pub); err == nil || !strings.Contains(err.Error(), "format") {
		t.Fatalf("schema 2: %v", err)
	}
}

func TestReleaseKey(t *testing.T) {
	dir := t.TempDir()
	key := base64.StdEncoding.EncodeToString(make([]byte, 32))
	writeFile(t, filepath.Join(dir, "launcher", "fh2mp_launcher", "constants.py"),
		"RELEASE_PUBKEY_B64 = os.environ.get(\"FH2MP_RELEASE_PUBKEY\") or \""+key+"\"\n")
	t.Setenv("FH2MP_RELEASE_PUBKEY", "")
	k, err := releaseKey(dir)
	if err != nil || len(k) != 32 {
		t.Fatalf("from constants: %v", err)
	}
	other := base64.StdEncoding.EncodeToString([]byte("0123456789abcdef0123456789abcdef"))
	t.Setenv("FH2MP_RELEASE_PUBKEY", other)
	k, err = releaseKey(dir)
	if err != nil || string(k) != "0123456789abcdef0123456789abcdef" {
		t.Fatalf("from the environment: %v", err)
	}
	t.Setenv("FH2MP_RELEASE_PUBKEY", "")
	// the checkout's own key
	if _, err := releaseKey("../../.."); err != nil {
		t.Fatalf("the launcher's constants: %v", err)
	}
}

func TestCompat(t *testing.T) {
	out, err := compat("../../..")
	if err != nil {
		t.Fatalf("the launcher's constants: %v", err)
	}
	raw, _ := base64.StdEncoding.DecodeString(out)
	var c struct {
		Latest    string  `json:"latestAzimuth"`
		Supported []build `json:"supportedAzimuth"`
	}
	if err := json.Unmarshal(raw, &c); err != nil || c.Latest == "" || len(c.Supported) == 0 || c.Supported[0].Version != c.Latest ||
		len(c.Supported[0].SHA256) != 64 {
		t.Fatalf("checkout: %s %v", raw, err)
	}

	dir := t.TempDir()
	path := filepath.Join(dir, "launcher", "fh2mp_launcher", "constants.py")
	h1, h2 := strings.Repeat("a", 64), strings.Repeat("b", 64)
	writeFile(t, path, "AZIMUTH_VERSION = \"1.0.5\"\nBUILTIN_AZIMUTH_BUILDS = [\n    {\"version\": AZIMUTH_VERSION, \"sha256\": \""+h1+"\"},\n"+
		"    {\"version\": \"1.0.4\", \"sha256\": \""+h2+"\"},\n]\n")
	out, err = compat(dir)
	raw, _ = base64.StdEncoding.DecodeString(out)
	if err != nil || string(raw) != `{"latestAzimuth":"1.0.5","supportedAzimuth":[{"version":"1.0.5","sha256":"`+h1+`"},{"version":"1.0.4","sha256":"`+h2+`"}]}` {
		t.Fatalf("two builds: %s %v", raw, err)
	}
	writeFile(t, path, "AZIMUTH_VERSION = \"1.0.5\"\nBUILTIN_AZIMUTH_BUILDS = [\n    {\"version\": AZIMUTH_VERSION, \"sha256\": \""+h1+"\", \"note\": 1},\n]\n")
	if _, err := compat(dir); err == nil {
		t.Fatal("an entry with another key was read")
	}
}

func TestGameDB(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "gamedb.slt")
	db, err := sql.Open("sqlite", "file:"+filepath.ToSlash(path))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec("CREATE TABLE Data_Car (Id INTEGER)"); err != nil {
		t.Fatal(err)
	}
	db.Close()
	if _, err := checkGameDB(path); err == nil || err.Error() != "empty car catalog" {
		t.Fatalf("empty: %v", err)
	}
	db, _ = sql.Open("sqlite", "file:"+filepath.ToSlash(path))
	if _, err := db.Exec("INSERT INTO Data_Car VALUES (1), (2), (3)"); err != nil {
		t.Fatal(err)
	}
	db.Close()
	out, err := checkGameDB(path)
	if err != nil || !strings.Contains(out, ", 3 cars, SHA256 ") {
		t.Fatalf("three cars: %q %v", out, err)
	}
	writeFile(t, filepath.Join(dir, "text.slt"), "not a database")
	if _, err := checkGameDB(filepath.Join(dir, "text.slt")); err == nil || err.Error() != "invalid canonical database" {
		t.Fatalf("text: %v", err)
	}
}

func TestGetAndConfigCheck(t *testing.T) {
	out, err := get(strings.NewReader(`{"minLauncher":"1.4.0","minShim":"","x":null}`), "0.0.0", []string{"minLauncher", "minShim", "x", "absent"})
	if err != nil || out != "1.4.0 0.0.0 0.0.0 0.0.0" {
		t.Fatalf("get: %q %v", out, err)
	}
	if _, err := get(strings.NewReader(`[1]`), "", []string{"a"}); err == nil {
		t.Fatal("an array was read as an object")
	}
	compatB64 := base64.StdEncoding.EncodeToString([]byte(`{"latestAzimuth":"1.0.4","supportedAzimuth":[{"version":"1.0.4","sha256":"ab"}]}`))
	cfg := `{"minLauncher":"1.4.1","minShim":"1.4.1","latestAzimuth":"1.0.4","supportedAzimuth":[{"sha256":"ab","version":"1.0.4"}],"maintenance":""}`
	out, err = configCheck(strings.NewReader(cfg), "1.4.1", "1.4.1", compatB64)
	if err != nil || out != "server reports minLauncher/minShim: ('1.4.1', '1.4.1')" {
		t.Fatalf("matching: %q %v", out, err)
	}
	if _, err := configCheck(strings.NewReader(cfg), "1.4.1", "1.4.0", compatB64); err == nil || !strings.HasPrefix(err.Error(), "the server does not report 1.4.1 / 1.4.0") {
		t.Fatalf("other minimum: %v", err)
	}
	stale := strings.Replace(cfg, `"latestAzimuth":"1.0.4"`, `"latestAzimuth":"1.0.3"`, 1)
	if _, err := configCheck(strings.NewReader(stale), "1.4.1", "1.4.1", compatB64); err == nil || err.Error() != "server reports stale Azimuth compatibility metadata" {
		t.Fatalf("stale: %v", err)
	}
}

func TestCommandLine(t *testing.T) {
	var u usageError
	for _, args := range [][]string{nil, {"nope"}, {"manifest", "a"}, {"gamedb"}, {"get"}, {"config-check", "1"}} {
		if _, err := run(args, strings.NewReader("")); !errors.As(err, &u) {
			t.Fatalf("%v: %v", args, err)
		}
	}
	if _, err := run([]string{"compat", "--root", "../../.."}, nil); err != nil {
		t.Fatalf("compat --root: %v", err)
	}
}
