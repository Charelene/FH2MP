package main

import (
	"archive/zip"
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// The v1.4.8 release's own manifest (make_manifest.py's bytes, signed with the release key): decoded and written again it is the same bytes,
// so a manifest this tool writes is what Python's json.dumps(indent=1, sort_keys=True) wrote, and the checkout's key verifies it.
func TestPyJSONIsMakeManifestsBytes(t *testing.T) {
	raw, err := os.ReadFile("testdata/release-v1.4.8/manifest.json")
	if err != nil {
		t.Fatal(err)
	}
	m, err := decodeObject(raw)
	if err != nil {
		t.Fatal(err)
	}
	got, err := pyJSON(m)
	if err != nil || !bytes.Equal(got, raw) {
		t.Fatalf("v1.4.8 written again differs (%v):\n%s", err, got)
	}
	sig, _ := os.ReadFile("testdata/release-v1.4.8/manifest.json.sig")
	s, _ := base64.StdEncoding.DecodeString(strings.TrimSpace(string(sig)))
	key, err := releaseKey("../../..")
	if err != nil || !ed25519.Verify(key, raw, s) {
		t.Fatalf("release/release-key.pub does not verify v1.4.8's manifest: %v", err)
	}
}

func TestPyJSONEscapesAsPython(t *testing.T) {
	// json.dumps({"b": [], "a": {}, "s": "\"\\\n\t\b\f\x01\x7f\xe9\U0001F600 ~", "n": [1, -2]}, indent=1, sort_keys=True)
	want := "{\n \"a\": {},\n \"b\": [],\n \"n\": [\n  1,\n  -2\n ],\n \"s\": \"\\\"\\\\\\n\\t\\b\\f\\u0001\\u007f\\u00e9\\ud83d\\ude00 ~\"\n}\n"
	got, err := pyJSON(map[string]any{"b": []any{}, "a": map[string]any{}, "s": "\"\\\n\t\b\f\x01\x7fé\U0001F600 ~",
		"n": []any{json.Number("1"), int64(-2)}})
	if err != nil || string(got) != want {
		t.Fatalf("got %q (%v)\nwant %q", got, err, want)
	}
	if _, err := pyJSON(map[string]any{"x": json.Number("1.5")}); err == nil {
		t.Fatal("a fraction was written")
	}
	if _, err := pyJSON(map[string]any{"x": 1.5}); err == nil {
		t.Fatal("a float was written")
	}
}

type tree struct {
	root, dist string
	pub        ed25519.PublicKey
	seed       string
}

func put(t *testing.T, p string, b []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, b, 0o644); err != nil {
		t.Fatal(err)
	}
}

func zipBytes(t *testing.T, files map[string][]byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	z := zip.NewWriter(&buf)
	var names []string
	for n := range files {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		w, err := z.Create(n)
		if err != nil {
			t.Fatal(err)
		}
		w.Write(files[n])
	}
	z.Close()
	return buf.Bytes()
}

func digest(b []byte) string { s := sha256.Sum256(b); return hex.EncodeToString(s[:]) }

var (
	setupExe = append([]byte("MZ"), bytes.Repeat([]byte{0x90}, 62)...)
	appImage = append([]byte("\x7fELF\x02\x01\x01\x00AI\x02"), bytes.Repeat([]byte{0}, 53)...)
	debFile  = append([]byte("!<arch>\ndebian-binary   "), bytes.Repeat([]byte{' '}, 40)...)
)

// a checkout (release key, Encore pin) and a release folder: the shim zip of version ver and Encore 2.1.0's files
func newTree(t *testing.T, ver string, dll []byte, portable []byte) tree {
	t.Helper()
	pub, prv, _ := ed25519.GenerateKey(rand.Reader)
	tr := tree{root: t.TempDir(), dist: t.TempDir(), pub: pub, seed: base64.StdEncoding.EncodeToString(prv.Seed())}
	put(t, filepath.Join(tr.root, "release", "release-key.pub"), []byte(base64.StdEncoding.EncodeToString(pub)+"\n"))
	if dll == nil {
		dll = []byte("MZ...FH2MP-SHIM-VERSION:" + ver + ":END...")
	}
	put(t, filepath.Join(tr.dist, "fh2mp-shim-"+ver+".zip"), zipBytes(t, map[string][]byte{"fh2-mods/FH2MP.dll": dll, "fh2mp/LICENSE.txt": []byte("GPL")}))
	if portable == nil {
		portable = zipBytes(t, map[string][]byte{"fh2mp-launcher.exe": setupExe})
	}
	files := map[string][]byte{"EncoreSetup-2.1.0.exe": setupExe, "Encore-2.1.0-x86_64.AppImage": appImage, "Encore-2.1.0-fh2mp-portable.zip": portable}
	for n, b := range files {
		put(t, filepath.Join(tr.dist, n), b)
	}
	tr.pin(t, "2.1.0", map[string]string{"windows-x64-installer": "EncoreSetup-2.1.0.exe", "linux-appimage": "Encore-2.1.0-x86_64.AppImage",
		"windows-x64-portable": "Encore-2.1.0-fh2mp-portable.zip"})
	return tr
}

// pin writes release/launcher.json for the files in dist (their real hashes and sizes)
func (tr tree) pin(t *testing.T, minLauncher string, assets map[string]string) {
	t.Helper()
	out := map[string]any{}
	for k, n := range assets {
		b, err := os.ReadFile(filepath.Join(tr.dist, n))
		if err != nil {
			t.Fatal(err)
		}
		out[k] = map[string]any{"name": n, "sha256": digest(b), "size": len(b)}
	}
	raw, _ := json.Marshal(map[string]any{"version": "2.1.0", "minLauncher": minLauncher, "repo": "Charelene/Encore", "tag": "v2.1.0", "assets": out})
	put(t, filepath.Join(tr.root, "release", "launcher.json"), raw)
}

func (tr tree) build(t *testing.T, args ...string) (string, error) {
	t.Helper()
	t.Setenv("FH2MP_RELEASE_KEY", tr.seed)
	t.Setenv("FH2MP_RELEASE_PUBKEY", "")
	return run(append([]string{"manifest-build", "--dist", tr.dist, "--root", tr.root}, args...), nil)
}

func TestManifestBuild(t *testing.T) {
	tr := newTree(t, "1.5.0", nil, nil)
	out, err := tr.build(t, "--tag", "v1.5.0", "--kind", "release")
	if err != nil || out != "manifest for v1.5.0 (release): shim 1.5.0, launcher 2.1.0 from Charelene/Encore v2.1.0 with 3 package(s): "+
		"linux-appimage, windows-x64-installer, windows-x64-portable; minLauncher 2.1.0" {
		t.Fatalf("build: %q %v", out, err)
	}
	raw, _ := os.ReadFile(filepath.Join(tr.dist, "manifest.json"))
	sigText, _ := os.ReadFile(filepath.Join(tr.dist, "manifest.json.sig"))
	sig, _ := base64.StdEncoding.DecodeString(strings.TrimSpace(string(sigText)))
	if !ed25519.Verify(tr.pub, raw, sig) || !bytes.HasSuffix(sigText, []byte("\n")) {
		t.Fatal("the signature does not verify the manifest's bytes")
	}
	m, err := decodeObject(raw)
	if err != nil {
		t.Fatal(err)
	}
	again, _ := pyJSON(m)
	if !bytes.Equal(again, raw) || !bytes.HasPrefix(raw, []byte("{\n \"kind\": \"release\",\n \"launcher\": {\n  \"assets\": {\n")) {
		t.Fatalf("not make_manifest.py's layout:\n%s", raw)
	}
	zipRaw, _ := os.ReadFile(filepath.Join(tr.dist, "fh2mp-shim-1.5.0.zip"))
	dll := []byte("MZ...FH2MP-SHIM-VERSION:1.5.0:END...")
	want := map[string]any{"schema": json.Number("1"), "tag": "v1.5.0", "version": "1.5.0", "line": "1.5", "kind": "release", "minLauncher": "2.1.0",
		"notes": "", "shim": map[string]any{"version": "1.5.0", "name": "fh2mp-shim-1.5.0.zip", "sha256": digest(zipRaw),
			"size": json.Number(fmt.Sprint(len(zipRaw))), "dllSha256": digest(dll)},
		"launcher": map[string]any{"version": "2.1.0", "assets": map[string]any{
			"windows-x64-installer": map[string]any{"name": "EncoreSetup-2.1.0.exe", "sha256": digest(setupExe), "size": json.Number(fmt.Sprint(len(setupExe)))},
			"linux-appimage":        map[string]any{"name": "Encore-2.1.0-x86_64.AppImage", "sha256": digest(appImage), "size": json.Number(fmt.Sprint(len(appImage)))},
			"windows-x64-portable":  m["launcher"].(map[string]any)["assets"].(map[string]any)["windows-x64-portable"],
		}}}
	if a, b := fmt.Sprint(m), fmt.Sprint(want); a != b {
		t.Fatalf("manifest\n got %s\nwant %s", a, b)
	}
	// what ops/release-prod.sh checks after the release is published
	var list []map[string]any
	for _, n := range []string{"fh2mp-shim-1.5.0.zip", "EncoreSetup-2.1.0.exe", "Encore-2.1.0-x86_64.AppImage", "Encore-2.1.0-fh2mp-portable.zip"} {
		st, _ := os.Stat(filepath.Join(tr.dist, n))
		list = append(list, map[string]any{"name": n, "size": st.Size()})
	}
	a, _ := json.Marshal(map[string]any{"assets": list})
	put(t, filepath.Join(tr.dist, "assets.json"), a)
	if out, err := run([]string{"manifest", tr.dist, "v1.5.0", "1.5.0", "2.1.0", "--root", tr.root}, nil); err != nil ||
		out != "manifest ok: linux-appimage, windows-x64-installer, windows-x64-portable" {
		t.Fatalf("check: %q %v", out, err)
	}

	// a launcher-only release carries the shim from its own release
	carry := t.TempDir()
	put(t, filepath.Join(carry, "v1.5.0", "manifest.json"), raw)
	put(t, filepath.Join(carry, "v1.5.0", "manifest.json.sig"), sigText)
	out, err = tr.build(t, "--tag", "v1.5.1", "--kind", "hotfix", "--shim-version", "1.5.0", "--carry-dir", carry)
	if err != nil || !strings.HasPrefix(out, "manifest for v1.5.1 (hotfix): shim 1.5.0 carried, launcher 2.1.0") {
		t.Fatalf("carried: %q %v", out, err)
	}
	raw2, _ := os.ReadFile(filepath.Join(tr.dist, "manifest.json"))
	m2, _ := decodeObject(raw2)
	if fmt.Sprint(m2["shim"]) != fmt.Sprint(m["shim"]) || m2["line"] != "1.5" || m2["tag"] != "v1.5.1" {
		t.Fatalf("carried manifest: %s", raw2)
	}
	put(t, filepath.Join(carry, "v1.5.0", "manifest.json"), bytes.Replace(raw, []byte(`"notes": ""`), []byte(`"notes": "x"`), 1))
	if _, err := tr.build(t, "--tag", "v1.5.1", "--kind", "hotfix", "--shim-version", "1.5.0", "--carry-dir", carry); err == nil ||
		!strings.Contains(err.Error(), "is not signed with this key") {
		t.Fatalf("a changed carried manifest: %v", err)
	}
	if _, err := tr.build(t, "--tag", "v1.5.1", "--kind", "hotfix", "--shim-version", "1.4.8", "--carry-dir", carry); err == nil ||
		!strings.Contains(err.Error(), "cannot read the manifest of v1.4.8") {
		t.Fatalf("nothing to carry: %v", err)
	}
}

func TestManifestBuildRefuses(t *testing.T) {
	check := func(what string, tr tree, want string, args ...string) {
		t.Helper()
		if len(args) == 0 {
			args = []string{"--tag", "v1.5.0", "--kind", "release"}
		}
		if _, err := tr.build(t, args...); err == nil || !strings.Contains(err.Error(), want) {
			t.Fatalf("%s: %v (want %q)", what, err, want)
		}
	}
	check("a debug shim", newTree(t, "1.5.0", []byte("FH2MP-SHIM-VERSION:1.5.0:END FH2MP-SHIM-VERSION:1.5.0-debug:END"), nil), "debug build")
	check("a research shim", newTree(t, "1.5.0", append([]byte("FH2MP-SHIM-VERSION:1.5.0:END"), utf16le("fh2mp-dbg")...), nil), "debug build")
	check("another version's shim", newTree(t, "1.5.0", []byte("FH2MP-SHIM-VERSION:1.4.8:END"), nil), "this version's marker")
	check("an unknown kind", newTree(t, "1.5.0", nil, nil), "kind \"full\"", "--tag", "v1.5.0", "--kind", "full")
	check("a bad tag", newTree(t, "1.5.0", nil, nil), "tag must look like v1.0.0", "--tag", "1.5.0", "--kind", "release")

	tr := newTree(t, "1.5.0", nil, nil)
	put(t, filepath.Join(tr.dist, "fh2mp-shim-1.5.0.zip"), zipBytes(t, map[string][]byte{"fh2-mods/FH2MP.dll": []byte("FH2MP-SHIM-VERSION:1.5.0:END"),
		"fh2-mods/FH2MPGuide.dll": []byte("MZ")}))
	check("a Guide in the shim zip", tr, "the Guide is Encore's")

	tr = newTree(t, "1.5.0", nil, nil)
	put(t, filepath.Join(tr.dist, "EncoreSetup-2.1.0.exe"), append(setupExe, 0)) // not the pinned file any more
	check("a launcher file other than the pinned one", tr, "is not the file the launcher pin")

	tr = newTree(t, "1.5.0", nil, zipBytes(t, map[string][]byte{"Encore/fh2mp-launcher.exe": setupExe, "Encore.exe": setupExe}))
	check("a portable zip without fh2mp-launcher.exe at its root", tr, "has no fh2mp-launcher.exe at its root")

	tr = newTree(t, "1.5.0", nil, nil)
	put(t, filepath.Join(tr.dist, "Encore-2.1.0-x86_64.AppImage"), setupExe)
	tr.pin(t, "2.1.0", map[string]string{"linux-appimage": "Encore-2.1.0-x86_64.AppImage"})
	check("an AppImage that is not one", tr, "is not a type 2 AppImage")

	tr = newTree(t, "1.5.0", nil, nil)
	tr.pin(t, "2.2.0", map[string]string{"windows-x64-installer": "EncoreSetup-2.1.0.exe"})
	check("a minLauncher above the launcher", tr, "minLauncher 2.2.0 is above the launcher it names")
	tr.pin(t, "2.1.0", map[string]string{"windows-x64-installer": "EncoreSetup-2.1.0.exe"})
	check("--min-launcher above the launcher", tr, "no higher than the launcher", "--tag", "v1.5.0", "--kind", "release", "--min-launcher", "2.1.1")

	tr = newTree(t, "1.5.0", nil, nil)
	put(t, filepath.Join(tr.root, "release", "launcher.json"), []byte(`{"version": "2.1.0", "minLauncher": "2.1.0", "repo": "Charelene/Encore",
		"tag": "v2.1.0", "assets": {"windows-x64-installer": {"name": "EncoreSetup-2.1.0.exe", "sha256": "", "size": 0}}}`))
	check("a pin without hashes", tr, "has no SHA-256 and size yet")
	put(t, filepath.Join(tr.root, "release", "launcher.json"), []byte(`{"version": "2.1.0", "minLauncher": "2.1.0", "repo": "Charelene/Encore",
		"tag": "v2.1.0", "assets": {"macos": {"name": "Encore.dmg", "sha256": "`+sha("a")+`", "size": 5}}}`))
	check("a flavour no launcher asks for", tr, "is not a flavour a 1.4.x launcher asks for")

	tr = newTree(t, "1.5.0", nil, nil)
	_, other, _ := ed25519.GenerateKey(rand.Reader)
	tr.seed = base64.StdEncoding.EncodeToString(other.Seed())
	check("a key that is not the release key", tr, "not the release key's private half")
	tr.seed = ""
	check("no key", tr, "no signing key")
}

func TestLauncherPinCommand(t *testing.T) {
	tr := newTree(t, "1.5.0", nil, nil)
	tr.pin(t, "2.1.0", map[string]string{"windows-x64-installer": "EncoreSetup-2.1.0.exe", "linux-appimage": "Encore-2.1.0-x86_64.AppImage"})
	for what, want := range map[string]string{
		"check":    "launcher pin ok: 2.1.0 from Charelene/Encore v2.1.0, minLauncher 2.1.0; linux-appimage, windows-x64-installer",
		"version":  "2.1.0",
		"min":      "2.1.0",
		"repo":     "Charelene/Encore",
		"tag":      "v2.1.0",
		"names":    "Encore-2.1.0-x86_64.AppImage EncoreSetup-2.1.0.exe",
		"flavours": "linux-appimage windows-x64-installer",
	} {
		if out, err := run([]string{"launcher-pin", what, "--root", tr.root}, nil); err != nil || out != want {
			t.Fatalf("%s: %q %v", what, out, err)
		}
	}
	var u usageError
	if _, err := run([]string{"launcher-pin", "nope", "--root", tr.root}, nil); err == nil || !strings.Contains(err.Error(), "usage") {
		t.Fatalf("unknown field: %v", err)
	}
	if _, err := run([]string{"launcher-pin"}, nil); !errors.As(err, &u) {
		t.Fatalf("no field: %v", err)
	}
}

func TestKeygenAndZip(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, "signing.key")
	out, err := keygen(file)
	if err != nil {
		t.Fatal(err)
	}
	text, _ := os.ReadFile(file)
	seed, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(text)))
	if err != nil || len(seed) != 32 {
		t.Fatalf("seed file: %q %v", text, err)
	}
	pub := ed25519.NewKeyFromSeed(seed).Public().(ed25519.PublicKey)
	if !strings.HasSuffix(out, "release/release-key.pub: "+base64.StdEncoding.EncodeToString(pub)) {
		t.Fatalf("printed key: %q", out)
	}
	if _, err := keygen(file); err == nil {
		t.Fatal("an existing key was overwritten")
	}

	put(t, filepath.Join(dir, "pkg", "fh2-mods", "FH2MP.dll"), []byte("dll"))
	put(t, filepath.Join(dir, "pkg", "fh2mp", "README.txt"), []byte("readme"))
	zp := filepath.Join(dir, "out.zip")
	if _, err := run([]string{"zip", zp, filepath.Join(dir, "pkg"), "fh2-mods/FH2MP.dll", "fh2mp/README.txt"}, nil); err != nil {
		t.Fatal(err)
	}
	z, err := zip.OpenReader(zp)
	if err != nil {
		t.Fatal(err)
	}
	defer z.Close()
	if len(z.File) != 2 || z.File[0].Name != "fh2-mods/FH2MP.dll" || z.File[1].Name != "fh2mp/README.txt" {
		t.Fatalf("zip members: %v", z.File)
	}
	if _, err := run([]string{"zip", zp, filepath.Join(dir, "pkg"), "../signing.key"}, nil); err == nil {
		t.Fatal("a file outside the folder was packed")
	}
}
