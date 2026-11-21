package main

// fh2mp-release launcher-pin set: release/launcher.json filled in from the files an Encore release published, so a new Encore version
// reaches this product by a reviewed change to the pin (release/bump-launcher.sh, .github/workflows/bump-launcher.yml) and not by hand.

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// The file each flavour a 1.4.x launcher asks for is in an Encore release (Encore's packaging/windows/build.ps1 and
// packaging/linux/build-appimage.sh name them); Encore ships no .deb, so linux-deb stays out of the pin.
var pinFiles = []struct{ flavour, name string }{
	{"linux-appimage", "Encore-%s-x86_64.AppImage"},
	{"windows-x64-installer", "EncoreSetup-%s.exe"},
	{"windows-x64-portable", "Encore-%s-windows-x64-portable.zip"},
}

// launcherPinSet: fh2mp-release launcher-pin set --tag vX.Y.Z --files DIR [--repo OWNER/NAME]. DIR holds the release's files as downloaded
// (gh release download); each is hashed here, never taken from the release's own manifest. The pin's about text and minLauncher stay as they
// are, the repository stays unless --repo names another, and a tag below the pinned version is refused: the pin only moves forward.
func launcherPinSet(dir string, args []string) (string, error) {
	const usage = "usage: fh2mp-release launcher-pin set --tag vX.Y.Z --files DIR [--repo OWNER/NAME] [--root DIR]"
	var tag, files, repo string
	for i := 0; i < len(args); i += 2 {
		if i+1 >= len(args) {
			return "", usageError(usage)
		}
		switch args[i] {
		case "--tag":
			tag = args[i+1]
		case "--files":
			files = args[i+1]
		case "--repo":
			repo = args[i+1]
		default:
			return "", usageError(usage)
		}
	}
	if tag == "" || files == "" {
		return "", usageError(usage)
	}
	version := strings.TrimPrefix(tag, "v")
	if !strings.HasPrefix(tag, "v") || !versionRe.MatchString(version) {
		return "", fmt.Errorf("tag %q must be vX.Y.Z or vX.Y.Z-rcN", tag)
	}
	pinPath := filepath.Join(dir, "release", "launcher.json")
	raw, err := os.ReadFile(pinPath)
	if err != nil {
		return "", fmt.Errorf("the launcher pin: %v", err)
	}
	var old struct {
		About       string `json:"about"`
		Version     string `json:"version"`
		MinLauncher string `json:"minLauncher"`
		Repo        string `json:"repo"`
	}
	if err := json.Unmarshal(raw, &old); err != nil {
		return "", fmt.Errorf("release/launcher.json is not readable: %v", err)
	}
	if repo == "" {
		repo = old.Repo
	}
	if !repoRe.MatchString(repo) {
		return "", fmt.Errorf("repo %q must be OWNER/NAME", repo)
	}
	if versionRe.MatchString(old.Version) && vkey(version) < vkey(old.Version) {
		return "", fmt.Errorf("release/launcher.json pins %s: %s is older, and the pin only moves forward", old.Version, version)
	}
	assets := map[string]pinAsset{}
	for _, f := range pinFiles {
		name := fmt.Sprintf(f.name, version)
		a, err := hashFile(filepath.Join(files, name))
		if err != nil {
			return "", fmt.Errorf("%s (%s) of Encore %s: %v", f.flavour, name, version, err)
		}
		a.Name = name
		assets[f.flavour] = a
	}

	var b strings.Builder
	b.WriteString("{\n")
	about, _ := json.Marshal(old.About)
	fmt.Fprintf(&b, " \"version\": %q,\n \"about\": %s,\n \"minLauncher\": %q,\n \"repo\": %q,\n \"tag\": %q,\n \"assets\": {\n",
		version, about, old.MinLauncher, repo, tag)
	for i, f := range pinFiles {
		a := assets[f.flavour]
		fmt.Fprintf(&b, "  %q: {\"name\": %q, \"sha256\": %q, \"size\": %d}", f.flavour, a.Name, a.SHA256, a.Size)
		if i < len(pinFiles)-1 {
			b.WriteString(",")
		}
		b.WriteString("\n")
	}
	b.WriteString(" }\n}\n")

	// written only if the result reads back as a complete pin (readPin: every field, the file names, sizes, minLauncher at most the version)
	tmp := pinPath + ".new"
	if err := os.WriteFile(tmp, []byte(b.String()), 0o644); err != nil {
		return "", err
	}
	if err := os.Rename(tmp, pinPath); err != nil {
		os.Remove(tmp)
		return "", err
	}
	if _, err := readPin(dir); err != nil {
		os.WriteFile(pinPath, raw, 0o644)
		return "", err
	}
	return fmt.Sprintf("launcher pin set: %s from %s %s", version, repo, tag), nil
}

func hashFile(path string) (pinAsset, error) {
	f, err := os.Open(path)
	if err != nil {
		return pinAsset{}, fmt.Errorf("not in the release's files")
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return pinAsset{}, err
	}
	return pinAsset{SHA256: hex.EncodeToString(h.Sum(nil)), Size: n}, nil
}
