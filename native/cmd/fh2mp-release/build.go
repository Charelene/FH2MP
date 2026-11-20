package main

// The release's own files: the launcher pin (release/launcher.json), the signed manifest (was release/make_manifest.py until 1.5.0, same
// rules and bytes), the release key pair (was release/keygen.py) and the shim zip (was a python3 one-liner in release/build_shim_zip.sh).

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
	"io"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"
)

// What a 1.4.x FH2MP Launcher can be (launcher/fh2mp_launcher/updater.py launcher_flavour at v1.4.8), each with what it does with its file
// (apply_launcher_update): the release has to hand each one a file it can use that way.
var flavours = []string{"linux-appimage", "linux-deb", "windows-x64-installer", "windows-x64-portable"}

// The exe a 1.4.x portable launcher is (its PyInstaller name): its updater copies the zip over its folder and starts that file again, so the
// zip must hold it at its root.
const portableExe = "fh2mp-launcher.exe"

var (
	nameRe    = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$`) // updater._NAME_RE
	sha256Re  = regexp.MustCompile(`^[0-9a-f]{64}$`)
	versionRe = regexp.MustCompile(`^[0-9]+\.[0-9]+\.[0-9]+(-rc[0-9]+)?$`) // what the LSP's versionAtLeast reads (debug builds aside)
	repoRe    = regexp.MustCompile(`^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$`)
	lineRe    = regexp.MustCompile(`^(\d+)\.(\d+)\.`)
)

const maxAsset = 400 * 1024 * 1024 // updater.MAX_ASSET: a 1.4.x launcher refuses a larger file

type pinAsset struct {
	Name   string `json:"name"`
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size"`
}

type launcherPin struct {
	Version     string              `json:"version"`
	MinLauncher string              `json:"minLauncher"`
	Repo        string              `json:"repo"`
	Tag         string              `json:"tag"`
	Assets      map[string]pinAsset `json:"assets"`
}

func (p *launcherPin) names() []string {
	var out []string
	for _, f := range flavours {
		if a, ok := p.Assets[f]; ok {
			out = append(out, a.Name)
		}
	}
	return out
}

func (p *launcherPin) keys() []string {
	var out []string
	for _, f := range flavours {
		if _, ok := p.Assets[f]; ok {
			out = append(out, f)
		}
	}
	return out
}

// readPin reads release/launcher.json: every field filled in, every flavour a known one with a file a 1.4.x launcher accepts by name and size,
// and a minLauncher the pinned launcher itself meets.
func readPin(dir string) (*launcherPin, error) {
	raw, err := os.ReadFile(filepath.Join(dir, "release", "launcher.json"))
	if err != nil {
		return nil, fmt.Errorf("the launcher pin: %v", err)
	}
	var p launcherPin
	if err := json.Unmarshal(raw, &p); err != nil {
		return nil, fmt.Errorf("release/launcher.json is not readable: %v", err)
	}
	if !versionRe.MatchString(p.Version) || !versionRe.MatchString(p.MinLauncher) {
		return nil, fmt.Errorf("release/launcher.json: version %q and minLauncher %q must be X.Y.Z or X.Y.Z-rcN", p.Version, p.MinLauncher)
	}
	if vkey(p.MinLauncher) > vkey(p.Version) {
		return nil, fmt.Errorf("release/launcher.json: minLauncher %s is above the launcher it names (%s), which would then be refused itself", p.MinLauncher, p.Version)
	}
	if !repoRe.MatchString(p.Repo) || !tagRe.MatchString(p.Tag) || !strings.HasPrefix(p.Tag, "v") {
		return nil, fmt.Errorf("release/launcher.json: repo %q and tag %q must name a GitHub repository and a vX.Y.Z tag", p.Repo, p.Tag)
	}
	if len(p.Assets) == 0 {
		return nil, errors.New("release/launcher.json names no launcher file")
	}
	known := map[string]bool{}
	for _, f := range flavours {
		known[f] = true
	}
	seen := map[string]string{}
	for k, a := range p.Assets {
		if !known[k] {
			return nil, fmt.Errorf("release/launcher.json: %q is not a flavour a 1.4.x launcher asks for (%s)", k, strings.Join(flavours, ", "))
		}
		if !nameRe.MatchString(a.Name) {
			return nil, fmt.Errorf("release/launcher.json: %s's file name %q is not one a 1.4.x launcher downloads", k, a.Name)
		}
		if other, dup := seen[a.Name]; dup {
			return nil, fmt.Errorf("release/launcher.json: %s and %s name the same file %s", other, k, a.Name)
		}
		seen[a.Name] = k
		if !sha256Re.MatchString(a.SHA256) || a.Size <= 0 {
			return nil, fmt.Errorf("release/launcher.json: %s (%s) has no SHA-256 and size yet: pin the file Encore's release %s published", k, a.Name, p.Tag)
		}
		if a.Size > maxAsset {
			return nil, fmt.Errorf("release/launcher.json: %s (%s) is %d bytes, more than a 1.4.x launcher downloads (%d)", k, a.Name, a.Size, maxAsset)
		}
	}
	return &p, nil
}

// launcherPinCmd: fh2mp-release launcher-pin check|version|min|repo|tag|names|flavours [--root DIR]
func launcherPinCmd(dir, what string) (string, error) {
	p, err := readPin(dir)
	if err != nil {
		return "", err
	}
	switch what {
	case "check":
		return fmt.Sprintf("launcher pin ok: %s from %s %s, minLauncher %s; %s", p.Version, p.Repo, p.Tag, p.MinLauncher, strings.Join(p.keys(), ", ")), nil
	case "version":
		return p.Version, nil
	case "min":
		return p.MinLauncher, nil
	case "repo":
		return p.Repo, nil
	case "tag":
		return p.Tag, nil
	case "names":
		return strings.Join(p.names(), " "), nil
	case "flavours":
		return strings.Join(p.keys(), " "), nil
	}
	return "", usageError("usage: fh2mp-release launcher-pin check|version|min|repo|tag|names|flavours [--root DIR]")
}

// vkey orders X.Y.Z and X.Y.Z-rcN (a release candidate before its release), as updater.vkey and the LSP's versionAtLeast do.
func vkey(v string) string {
	m := verKeyRe.FindStringSubmatch(v)
	if m == nil {
		return ""
	}
	n := func(s string) int { i, _ := strconv.Atoi(s); return i }
	stable, rc := 1, 0
	if m[4] != "" {
		stable, rc = 0, n(m[4])
	}
	return fmt.Sprintf("%09d.%09d.%09d.%d.%09d", n(m[1]), n(m[2]), n(m[3]), stable, rc)
}

var verKeyRe = regexp.MustCompile(`^v?(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?$`)

// fileEntry: {name, sha256, size} of a file, as the manifest lists it.
func fileEntry(p string) (map[string]any, error) {
	f, err := os.Open(p)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return nil, err
	}
	return map[string]any{"name": filepath.Base(p), "sha256": hex.EncodeToString(h.Sum(nil)), "size": n}, nil
}

// sameFile: dist holds the file `e` names with exactly its SHA-256 and size.
func sameFile(dist string, name, sha string, size int64, what string) error {
	got, err := fileEntry(filepath.Join(dist, name))
	if err != nil {
		return fmt.Errorf("the %s %s is not in %s: %v", what, name, dist, err)
	}
	if got["sha256"] != sha || got["size"] != size {
		return fmt.Errorf("%s in %s is not the file the %s names (SHA-256 %s, %d bytes; expected %s, %d bytes)", name, dist, what,
			got["sha256"], got["size"], sha, size)
	}
	return nil
}

// zipFiles reads a zip: its member names with "/" (Windows' Compress-Archive may write "\") -> contents.
func zipFiles(p string) (map[string][]byte, error) {
	z, err := zip.OpenReader(p)
	if err != nil {
		return nil, err
	}
	defer z.Close()
	out := map[string][]byte{}
	for _, f := range z.File {
		if f.FileInfo().IsDir() {
			continue
		}
		r, err := f.Open()
		if err != nil {
			return nil, err
		}
		b, err := io.ReadAll(io.LimitReader(r, maxAsset+1))
		r.Close()
		if err != nil {
			return nil, err
		}
		out[strings.ReplaceAll(f.Name, `\`, "/")] = b
	}
	return out, nil
}

// buildShim: the manifest entry of the shim built for this release (build_shim in make_manifest.py): the zip holds fh2-mods/FH2MP.dll
// carrying this version's marker, and no research build. Since 1.5.0 the zip holds no Guide (Encore installs its own).
func buildShim(dist, ver string) (map[string]any, error) {
	p := filepath.Join(dist, "fh2mp-shim-"+ver+".zip")
	files, err := zipFiles(p)
	if err != nil {
		return nil, fmt.Errorf("missing or unreadable %s: %v", p, err)
	}
	dll, ok := files["fh2-mods/FH2MP.dll"]
	if !ok {
		return nil, fmt.Errorf("%s has no fh2-mods/FH2MP.dll: build it with release/build_shim_zip.sh", p)
	}
	if _, guide := files["fh2-mods/FH2MPGuide.dll"]; guide {
		return nil, fmt.Errorf("%s holds fh2-mods/FH2MPGuide.dll: the Guide is Encore's since 1.5.0 and is not in the shim zip", p)
	}
	if !bytes.Contains(dll, []byte("FH2MP-SHIM-VERSION:"+ver+":END")) {
		return nil, errors.New("the shim in the zip does not carry this version's marker: rebuild it with shim/VERSION = " + ver)
	}
	for _, marker := range [][]byte{[]byte("-debug:END"), utf16le("fh2mp-dbg"), []byte("fh2mp-dbg")} { // a research build must never be published
		if bytes.Contains(dll, marker) {
			return nil, errors.New("the shim in the zip is a debug build (live debugger / tracer): rebuild with shim/build.sh (no argument)")
		}
	}
	e, err := fileEntry(p)
	if err != nil {
		return nil, err
	}
	sum := sha256.Sum256(dll)
	e["version"] = ver
	e["dllSha256"] = hex.EncodeToString(sum[:])
	return e, nil
}

func utf16le(s string) []byte {
	var b []byte
	for _, r := range s {
		b = append(b, byte(r), byte(r>>8))
	}
	return b
}

// checkLauncherFile: the file can do what the 1.4.x launcher of that flavour does with it (updater.apply_launcher_update).
func checkLauncherFile(flavour, p string) error {
	head := make([]byte, 16)
	f, err := os.Open(p)
	if err != nil {
		return err
	}
	_, err = io.ReadFull(f, head)
	f.Close()
	if err != nil {
		return fmt.Errorf("%s: %v", p, err)
	}
	switch flavour {
	case "windows-x64-installer": // started as an installer with /SILENT /SUPPRESSMSGBOXES /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS
		if !bytes.HasPrefix(head, []byte("MZ")) {
			return fmt.Errorf("%s (windows-x64-installer) is not a Windows program", filepath.Base(p))
		}
	case "linux-appimage": // copied over $APPIMAGE and started from there
		if !bytes.HasPrefix(head, []byte("\x7fELF")) || !bytes.Equal(head[8:11], []byte("AI\x02")) {
			return fmt.Errorf("%s (linux-appimage) is not a type 2 AppImage", filepath.Base(p))
		}
	case "linux-deb": // pkexec apt-get install -y <file>
		if !bytes.HasPrefix(head, []byte("!<arch>\ndebian")) {
			return fmt.Errorf("%s (linux-deb) is not a Debian package", filepath.Base(p))
		}
	case "windows-x64-portable": // unpacked over the portable folder, then <folder>\fh2mp-launcher.exe is started
		z, err := zip.OpenReader(p)
		if err != nil {
			return fmt.Errorf("%s (windows-x64-portable) is not a zip: %v", filepath.Base(p), err)
		}
		defer z.Close()
		for _, m := range z.File {
			n := strings.ReplaceAll(m.Name, `\`, "/")
			if strings.EqualFold(n, portableExe) {
				return nil
			}
		}
		return fmt.Errorf("%s (windows-x64-portable) has no %s at its root: a 1.4.x portable launcher would start its old exe again", filepath.Base(p), portableExe)
	}
	return nil
}

// carried: the signed manifest of release v<ver> in carryDir/v<ver>/, checked with pub (make_manifest.py carried).
func carried(carryDir, ver string, pub ed25519.PublicKey) (map[string]any, error) {
	d := filepath.Join(carryDir, "v"+ver)
	raw, err := os.ReadFile(filepath.Join(d, "manifest.json"))
	if err != nil {
		return nil, fmt.Errorf("cannot read the manifest of v%s to carry from (%v): download it into %s", ver, err, d)
	}
	sigText, err := os.ReadFile(filepath.Join(d, "manifest.json.sig"))
	if err != nil {
		return nil, fmt.Errorf("cannot read the manifest of v%s to carry from (%v): download it into %s", ver, err, d)
	}
	sig, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(sigText)))
	if err != nil || !ed25519.Verify(pub, raw, sig) {
		return nil, fmt.Errorf("the manifest of v%s in %s is not signed with this key", ver, d)
	}
	m, err := decodeObject(raw)
	if err != nil {
		return nil, fmt.Errorf("the manifest of v%s in %s is not readable: %v", ver, d, err)
	}
	if m["tag"] != "v"+ver {
		return nil, fmt.Errorf("%s/manifest.json belongs to %v, not v%s", d, m["tag"], ver)
	}
	return m, nil
}

func decodeObject(raw []byte) (map[string]any, error) {
	d := json.NewDecoder(bytes.NewReader(raw))
	d.UseNumber() // sizes keep their digits, as Python's json keeps an int
	var m map[string]any
	if err := d.Decode(&m); err != nil || m == nil {
		return nil, errors.New("not a JSON object")
	}
	return m, nil
}

// signingKey: the base64 seed from the file, else from FH2MP_RELEASE_KEY; its public half must be the release key (release/release-key.pub
// or FH2MP_RELEASE_PUBKEY), so a manifest that no launcher would accept is never written.
func signingKey(file, dir string) (ed25519.PrivateKey, error) {
	text := os.Getenv("FH2MP_RELEASE_KEY")
	if file != "" {
		b, err := os.ReadFile(file)
		if err != nil {
			return nil, fmt.Errorf("the signing key: %v", err)
		}
		text = string(b)
	}
	text = strings.TrimSpace(text)
	if text == "" {
		return nil, errors.New("no signing key: pass --key or set FH2MP_RELEASE_KEY")
	}
	seed, err := base64.StdEncoding.DecodeString(text)
	if err != nil || len(seed) != ed25519.SeedSize {
		return nil, errors.New("the signing key is not a base64 Ed25519 seed (fh2mp-release keygen)")
	}
	sk := ed25519.NewKeyFromSeed(seed)
	want, err := releaseKey(dir)
	if err != nil {
		return nil, err
	}
	if !bytes.Equal(sk.Public().(ed25519.PublicKey), want) {
		return nil, errors.New("the signing key is not the release key's private half (release/release-key.pub): launchers would refuse what it signs")
	}
	return sk, nil
}

type buildArgs struct {
	tag, dist, shimVersion, kind, carryDir, minLauncher, notes, key, root string
}

// manifestBuild writes manifest.json and manifest.json.sig into the release folder: the shim built for this tag (or carried from its own
// release), the launcher pinned in release/launcher.json, each file checked; the signature is Ed25519 over the exact bytes of manifest.json.
func manifestBuild(a buildArgs) (string, error) {
	if !regexp.MustCompile(`^v[0-9][0-9A-Za-z.+-]*$`).MatchString(a.tag) || !tagRe.MatchString(a.tag) {
		return "", errors.New("tag must look like v1.0.0")
	}
	ver := a.tag[1:]
	switch a.kind {
	case "release", "hotfix", "rollback":
	default:
		return "", fmt.Errorf("kind %q is not release, hotfix or rollback (release/versions.sh)", a.kind)
	}
	sk, err := signingKey(a.key, a.root)
	if err != nil {
		return "", err
	}
	shimVer := a.shimVersion
	if shimVer == "" {
		shimVer = ver
	}
	var shim map[string]any
	if shimVer == ver {
		if shim, err = buildShim(a.dist, ver); err != nil {
			return "", err
		}
	} else {
		src, err := carried(a.carryDir, shimVer, sk.Public().(ed25519.PublicKey))
		if err != nil {
			return "", err
		}
		s, _ := src["shim"].(map[string]any)
		if s == nil || s["version"] != shimVer {
			return "", fmt.Errorf("the manifest of v%s names shim %v, not %s", shimVer, s["version"], shimVer)
		}
		name, _ := s["name"].(string)
		sha, _ := s["sha256"].(string)
		size, _ := s["size"].(json.Number)
		n, err := size.Int64()
		if err != nil || !nameRe.MatchString(name) {
			return "", fmt.Errorf("the shim entry of v%s is not readable", shimVer)
		}
		if err := sameFile(a.dist, name, sha, n, "carried shim"); err != nil {
			return "", err
		}
		shim = s
	}

	pin, err := readPin(a.root)
	if err != nil {
		return "", err
	}
	assets := map[string]any{}
	for _, k := range pin.keys() {
		e := pin.Assets[k]
		if err := sameFile(a.dist, e.Name, e.SHA256, e.Size, "launcher pin (release/launcher.json "+k+")"); err != nil {
			return "", err
		}
		if err := checkLauncherFile(k, filepath.Join(a.dist, e.Name)); err != nil {
			return "", err
		}
		assets[k] = map[string]any{"name": e.Name, "sha256": e.SHA256, "size": e.Size}
	}
	minLauncher := a.minLauncher
	if minLauncher == "" {
		minLauncher = pin.MinLauncher
	}
	if !versionRe.MatchString(minLauncher) || vkey(minLauncher) > vkey(pin.Version) {
		return "", fmt.Errorf("minLauncher %q must be a version no higher than the launcher this manifest names (%s)", minLauncher, pin.Version)
	}
	line := ""
	if m := lineRe.FindStringSubmatch(ver); m != nil {
		line = m[1] + "." + m[2]
	}
	m := map[string]any{"schema": 1, "tag": a.tag, "version": ver, "line": line, "kind": a.kind, "minLauncher": minLauncher, "notes": a.notes,
		"shim": shim, "launcher": map[string]any{"version": pin.Version, "assets": assets}}
	raw, err := pyJSON(m)
	if err != nil {
		return "", err
	}
	if err := os.WriteFile(filepath.Join(a.dist, "manifest.json"), raw, 0o644); err != nil {
		return "", err
	}
	sig := base64.StdEncoding.EncodeToString(ed25519.Sign(sk, raw)) + "\n"
	if err := os.WriteFile(filepath.Join(a.dist, "manifest.json.sig"), []byte(sig), 0o644); err != nil {
		return "", err
	}
	how := ""
	if shimVer != ver {
		how = " carried"
	}
	return fmt.Sprintf("manifest for %s (%s): shim %s%s, launcher %s from %s %s with %d package(s): %s; minLauncher %s", a.tag, a.kind, shimVer, how,
		pin.Version, pin.Repo, pin.Tag, len(assets), strings.Join(pin.keys(), ", "), minLauncher), nil
}

func parseBuildArgs(args []string, root string) (buildArgs, error) {
	a := buildArgs{root: root}
	fields := map[string]*string{"--tag": &a.tag, "--dist": &a.dist, "--shim-version": &a.shimVersion, "--kind": &a.kind, "--carry-dir": &a.carryDir,
		"--min-launcher": &a.minLauncher, "--notes": &a.notes, "--key": &a.key}
	for i := 0; i < len(args); i++ {
		p, ok := fields[args[i]]
		if !ok || i+1 >= len(args) {
			return a, usageError("usage: fh2mp-release manifest-build --tag vX.Y.Z --dist DIR --kind release|hotfix|rollback [--shim-version V] " +
				"[--carry-dir DIR] [--min-launcher V] [--notes TEXT] [--key FILE] [--root DIR]")
		}
		*p = args[i+1]
		i++
	}
	if a.tag == "" || a.dist == "" || a.kind == "" {
		return a, usageError("manifest-build needs --tag, --dist and --kind")
	}
	return a, nil
}

// pyJSON is Python's json.dumps(v, indent=1, sort_keys=True) + "\n", the bytes make_manifest.py signed: keys sorted, one space per level,
// ", " never (with an indent Python ends an item with "," and a newline), ": " after a key, and every character outside printable ASCII
// written as \uXXXX (ensure_ascii).
func pyJSON(v any) ([]byte, error) {
	var b strings.Builder
	if err := pyValue(&b, v, 0); err != nil {
		return nil, err
	}
	b.WriteString("\n")
	return []byte(b.String()), nil
}

func pyValue(b *strings.Builder, v any, depth int) error {
	indent := func(n int) { b.WriteString(strings.Repeat(" ", n)) }
	switch x := v.(type) {
	case nil:
		b.WriteString("null")
	case bool:
		b.WriteString(strconv.FormatBool(x))
	case string:
		pyString(b, x)
	case int:
		b.WriteString(strconv.Itoa(x))
	case int64:
		b.WriteString(strconv.FormatInt(x, 10))
	case json.Number:
		if _, err := x.Int64(); err != nil {
			return fmt.Errorf("the manifest holds a number that is not an integer (%s)", x)
		}
		b.WriteString(x.String())
	case map[string]any:
		if len(x) == 0 {
			b.WriteString("{}")
			return nil
		}
		keys := make([]string, 0, len(x))
		for k := range x {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		b.WriteString("{\n")
		for i, k := range keys {
			indent(depth + 1)
			pyString(b, k)
			b.WriteString(": ")
			if err := pyValue(b, x[k], depth+1); err != nil {
				return err
			}
			if i < len(keys)-1 {
				b.WriteString(",")
			}
			b.WriteString("\n")
		}
		indent(depth)
		b.WriteString("}")
	case []any:
		if len(x) == 0 {
			b.WriteString("[]")
			return nil
		}
		b.WriteString("[\n")
		for i, e := range x {
			indent(depth + 1)
			if err := pyValue(b, e, depth+1); err != nil {
				return err
			}
			if i < len(x)-1 {
				b.WriteString(",")
			}
			b.WriteString("\n")
		}
		indent(depth)
		b.WriteString("]")
	default:
		return fmt.Errorf("the manifest holds a %T, which it never should", v)
	}
	return nil
}

func pyString(b *strings.Builder, s string) {
	b.WriteByte('"')
	for len(s) > 0 {
		r, n := utf8.DecodeRuneInString(s)
		s = s[n:]
		switch r {
		case '"':
			b.WriteString(`\"`)
		case '\\':
			b.WriteString(`\\`)
		case '\n':
			b.WriteString(`\n`)
		case '\r':
			b.WriteString(`\r`)
		case '\t':
			b.WriteString(`\t`)
		case '\b':
			b.WriteString(`\b`)
		case '\f':
			b.WriteString(`\f`)
		default:
			switch {
			case r >= 0x20 && r <= 0x7e:
				b.WriteRune(r)
			case r < 0x10000:
				fmt.Fprintf(b, `\u%04x`, r)
			default:
				r -= 0x10000
				fmt.Fprintf(b, `\u%04x\u%04x`, 0xd800|(r>>10), 0xdc00|(r&0x3ff))
			}
		}
	}
	b.WriteByte('"')
}

// keygen writes a new release key pair's private seed (base64, 0600) to a file that must not exist yet and returns the public half's line
// for release/release-key.pub.
func keygen(file string) (string, error) {
	pub, prv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return "", err
	}
	f, err := os.OpenFile(file, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return "", fmt.Errorf("%v; refusing to overwrite a signing key", err)
	}
	_, werr := f.WriteString(base64.StdEncoding.EncodeToString(prv.Seed()) + "\n")
	if cerr := f.Close(); werr == nil {
		werr = cerr
	}
	if werr != nil {
		return "", werr
	}
	return fmt.Sprintf("private key written to %s (keep a backup; changing keys needs a launcher that trusts the new one)\nrelease/release-key.pub: %s",
		file, base64.StdEncoding.EncodeToString(pub)), nil
}

// zipDir packs files (paths relative to dir, "/" separated) into out, in that order, deflated, with the paths as given.
func zipDir(out, dir string, files []string) (string, error) {
	f, err := os.Create(out)
	if err != nil {
		return "", err
	}
	z := zip.NewWriter(f)
	for _, name := range files {
		name = path.Clean(filepath.ToSlash(name))
		if strings.HasPrefix(name, "../") || path.IsAbs(name) {
			z.Close()
			f.Close()
			return "", fmt.Errorf("%s is not inside %s", name, dir)
		}
		src, err := os.ReadFile(filepath.Join(dir, filepath.FromSlash(name)))
		if err == nil {
			var w io.Writer
			if w, err = z.Create(name); err == nil {
				_, err = w.Write(src)
			}
		}
		if err != nil {
			z.Close()
			f.Close()
			return "", err
		}
	}
	if err := z.Close(); err != nil {
		f.Close()
		return "", err
	}
	if err := f.Close(); err != nil {
		return "", err
	}
	return out, nil
}
