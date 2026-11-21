package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const pinBefore = `{
 "version": "2.1.0",
 "about": "kept \"as is\"",
 "minLauncher": "2.1.0",
 "repo": "Charelene/FH3MP",
 "tag": "v0.1.2",
 "assets": {
  "linux-appimage": {"name": "Encore-2.1.0-x86_64.AppImage", "sha256": "bb53a760bddb62f644b847c5afb9868d762dd3f136124d13d1f609fb17a9cadb", "size": 20417016},
  "windows-x64-installer": {"name": "EncoreSetup-2.1.0.exe", "sha256": "265c41e0b63fada19c65601ceb68bc4990074d45170d957f69c83a5d7705dfe7", "size": 19057360},
  "windows-x64-portable": {"name": "Encore-2.1.0-windows-x64-portable.zip", "sha256": "fa8e72eef28c53e351fc494efa36fb55410911763c4e56f3e44aa1a4fd624bf1", "size": 26033851}
 }
}
`

func pinTree(t *testing.T) (root, files string) {
	t.Helper()
	root = t.TempDir()
	files = t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "release"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "release", "launcher.json"), []byte(pinBefore), 0o644); err != nil {
		t.Fatal(err)
	}
	return
}

func putFile(t *testing.T, dir, name, body string) string {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256([]byte(body))
	return hex.EncodeToString(sum[:])
}

func TestLauncherPinSet(t *testing.T) {
	root, files := pinTree(t)
	want := map[string]string{
		"linux-appimage":        putFile(t, files, "Encore-2.1.3-x86_64.AppImage", "appimage 2.1.3"),
		"windows-x64-installer": putFile(t, files, "EncoreSetup-2.1.3.exe", "setup 2.1.3"),
		"windows-x64-portable":  putFile(t, files, "Encore-2.1.3-windows-x64-portable.zip", "portable 2.1.3"),
	}
	out, err := run([]string{"launcher-pin", "set", "--tag", "v2.1.3", "--repo", "Charelene/Encore", "--files", files, "--root", root}, nil)
	if err != nil || out != "launcher pin set: 2.1.3 from Charelene/Encore v2.1.3" {
		t.Fatalf("set: %q %v", out, err)
	}
	p, err := readPin(root)
	if err != nil {
		t.Fatal(err)
	}
	if p.Version != "2.1.3" || p.MinLauncher != "2.1.0" || p.Repo != "Charelene/Encore" || p.Tag != "v2.1.3" {
		t.Fatalf("fields: %+v", p)
	}
	for flavour, sum := range want {
		if p.Assets[flavour].SHA256 != sum || p.Assets[flavour].Size <= 0 {
			t.Fatalf("%s: %+v, want %s", flavour, p.Assets[flavour], sum)
		}
	}
	raw, _ := os.ReadFile(filepath.Join(root, "release", "launcher.json"))
	if !strings.Contains(string(raw), `"about": "kept \"as is\""`) {
		t.Fatalf("about changed:\n%s", raw)
	}
	// the same call again changes nothing
	before := string(raw)
	if _, err := run([]string{"launcher-pin", "set", "--tag", "v2.1.3", "--files", files, "--root", root}, nil); err != nil {
		t.Fatal(err)
	}
	if again, _ := os.ReadFile(filepath.Join(root, "release", "launcher.json")); string(again) != before {
		t.Fatalf("a second set changed the pin:\n%s", again)
	}
}

func TestLauncherPinSetRefuses(t *testing.T) {
	root, files := pinTree(t)
	putFile(t, files, "Encore-2.1.3-x86_64.AppImage", "a")
	putFile(t, files, "EncoreSetup-2.1.3.exe", "s")
	// a flavour's file missing: nothing is written
	if _, err := run([]string{"launcher-pin", "set", "--tag", "v2.1.3", "--files", files, "--root", root}, nil); err == nil ||
		!strings.Contains(err.Error(), "windows-x64-portable") {
		t.Fatalf("missing portable zip: %v", err)
	}
	putFile(t, files, "Encore-2.1.3-windows-x64-portable.zip", "p")
	for name, args := range map[string][]string{
		"older tag": {"--tag", "v2.0.9", "--files", files},
		"no v":      {"--tag", "2.1.3", "--files", files},
		"bad repo":  {"--tag", "v2.1.3", "--repo", "nope", "--files", files},
		"no files":  {"--tag", "v2.1.3"},
		"odd args":  {"--tag"},
		"unknown":   {"--tag", "v2.1.3", "--files", files, "--x", "y"},
		"empty dir": {"--tag", "v2.1.4", "--files", t.TempDir()},
	} {
		if _, err := run(append([]string{"launcher-pin", "set"}, append(args, "--root", root)...), nil); err == nil {
			t.Fatalf("%s: accepted", name)
		}
	}
	if raw, _ := os.ReadFile(filepath.Join(root, "release", "launcher.json")); string(raw) != pinBefore {
		t.Fatalf("a refused set changed the pin:\n%s", raw)
	}
}
