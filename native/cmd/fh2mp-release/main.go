// Command fh2mp-release holds the checks of the release scripts (release/publish.sh, ops/ship-hotfix.sh, ops/ship.sh, ops/release-prod.sh) that
// need more than the shell: the signed manifest of a GitHub release, the canonical car database, the Azimuth versions the launcher supports and
// the launcher config the LSP reports. Until 2026-11-14 these were Python one-liners in those scripts; the rules and messages are the same.
//
//	fh2mp-release manifest <dir> <tag> <shim version> <launcher version> [--root DIR]
//	    dir holds manifest.json, manifest.json.sig and assets.json (gh release view --json assets). The signature is checked with the launcher's
//	    release key (RELEASE_PUBKEY_B64 in launcher/fh2mp_launcher/constants.py, or FH2MP_RELEASE_PUBKEY as the launcher does).
//	fh2mp-release gamedb <file>
//	    the canonical car database: a SQLite file of at most 64 MB whose quick_check passes and whose Data_Car has rows.
//	fh2mp-release compat [--root DIR]
//	    base64 of {"latestAzimuth": AZIMUTH_VERSION, "supportedAzimuth": BUILTIN_AZIMUTH_BUILDS} from the launcher's constants.
//	fh2mp-release get [--default D] <key>...
//	    the JSON object on stdin: each key's value, separated by spaces (missing, null or "" gives D).
//	fh2mp-release config-check <min launcher> <min shim> <compat base64>
//	    the launcher config on stdin (GET /v1/launcher/config) reports those minimums and that compatibility metadata.
//
// Exit 1 with the reason when a check fails, 2 for a wrong command line.
package main

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"sort"
	"strings"

	_ "modernc.org/sqlite"
)

type usageError string

func (e usageError) Error() string { return string(e) }

func main() {
	out, err := run(os.Args[1:], os.Stdin)
	if out != "" {
		fmt.Println(out)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		var u usageError
		if errors.As(err, &u) {
			os.Exit(2)
		}
		os.Exit(1)
	}
}

// root takes --root DIR out of args (default: the checkout this binary was run from, i.e. the current directory). -> args, root
func root(args []string) ([]string, string) {
	r := "."
	var rest []string
	for i := 0; i < len(args); i++ {
		if args[i] == "--root" && i+1 < len(args) {
			r = args[i+1]
			i++
			continue
		}
		rest = append(rest, args[i])
	}
	return rest, r
}

func run(args []string, stdin io.Reader) (string, error) {
	if len(args) == 0 {
		return "", usageError("usage: fh2mp-release manifest|gamedb|compat|get|config-check ...")
	}
	cmd, args := args[0], args[1:]
	args, dir := root(args)
	switch cmd {
	case "manifest":
		if len(args) != 4 {
			return "", usageError("usage: fh2mp-release manifest <dir> <tag> <shim version> <launcher version> [--root DIR]")
		}
		key, err := releaseKey(dir)
		if err != nil {
			return "", err
		}
		return checkManifest(args[0], args[1], args[2], args[3], key)
	case "gamedb":
		if len(args) != 1 {
			return "", usageError("usage: fh2mp-release gamedb <file>")
		}
		return checkGameDB(args[0])
	case "compat":
		if len(args) != 0 {
			return "", usageError("usage: fh2mp-release compat [--root DIR]")
		}
		return compat(dir)
	case "get":
		def := ""
		if len(args) >= 2 && args[0] == "--default" {
			def, args = args[1], args[2:]
		}
		if len(args) == 0 {
			return "", usageError("usage: fh2mp-release get [--default D] <key>...")
		}
		return get(stdin, def, args)
	case "config-check":
		if len(args) != 3 {
			return "", usageError("usage: fh2mp-release config-check <min launcher> <min shim> <compat base64>")
		}
		return configCheck(stdin, args[0], args[1], args[2])
	}
	return "", usageError("unknown command " + cmd)
}

func readConstants(dir string) (string, error) {
	b, err := os.ReadFile(filepath.Join(dir, "launcher", "fh2mp_launcher", "constants.py"))
	if err != nil {
		return "", fmt.Errorf("the launcher's constants: %v", err)
	}
	return string(b), nil
}

var pubkeyRe = regexp.MustCompile(`(?m)^RELEASE_PUBKEY_B64\s*=.*?"([A-Za-z0-9+/]{43}=)"`)

// releaseKey: FH2MP_RELEASE_PUBKEY when set (the launcher reads it the same way), else the literal in constants.py.
func releaseKey(dir string) (ed25519.PublicKey, error) {
	text := os.Getenv("FH2MP_RELEASE_PUBKEY")
	if text == "" {
		c, err := readConstants(dir)
		if err != nil {
			return nil, err
		}
		m := pubkeyRe.FindStringSubmatch(c)
		if m == nil {
			return nil, errors.New("RELEASE_PUBKEY_B64 not found in launcher/fh2mp_launcher/constants.py")
		}
		text = m[1]
	}
	k, err := base64.StdEncoding.DecodeString(text)
	if err != nil || len(k) != ed25519.PublicKeySize {
		return nil, errors.New("the release key is not a base64 Ed25519 public key")
	}
	return ed25519.PublicKey(k), nil
}

var tagRe = regexp.MustCompile(`^v?[0-9][0-9A-Za-z.+-]{0,30}$`) // updater._TAG_RE

type asset struct {
	Name string `json:"name"`
	Size int64  `json:"size"`
}

// checkManifest: what ops/release-prod.sh needs before it raises the minimums: the manifest is signed with the release key, names this tag
// and these versions, lists the four launcher packages, and every file it names is published at its size.
func checkManifest(dir, tag, shim, launcher string, key ed25519.PublicKey) (string, error) {
	raw, err := os.ReadFile(filepath.Join(dir, "manifest.json"))
	if err != nil {
		return "", err
	}
	sigText, err := os.ReadFile(filepath.Join(dir, "manifest.json.sig"))
	if err != nil {
		return "", err
	}
	sig, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(sigText)))
	if err != nil || !ed25519.Verify(key, raw, sig) {
		return "", errors.New("the release manifest signature is not valid")
	}
	var m struct {
		Schema int    `json:"schema"`
		Tag    string `json:"tag"`
		Shim   *struct {
			Version string `json:"version"`
			asset
		} `json:"shim"`
		Launcher *struct {
			Version string           `json:"version"`
			Assets  map[string]asset `json:"assets"`
		} `json:"launcher"`
	}
	if err := json.Unmarshal(raw, &m); err != nil {
		return "", errors.New("the release manifest is not readable")
	}
	if m.Schema != 1 || !tagRe.MatchString(m.Tag) {
		return "", errors.New("the release manifest is not in a format this launcher understands")
	}
	if m.Tag != tag {
		return "", errors.New("manifest tag mismatch")
	}
	if m.Shim == nil || m.Shim.Version != shim {
		got := ""
		if m.Shim != nil {
			got = m.Shim.Version
		}
		return "", fmt.Errorf("manifest shim version '%s', expected %s", got, shim)
	}
	if m.Launcher == nil || m.Launcher.Version != launcher {
		got := ""
		if m.Launcher != nil {
			got = m.Launcher.Version
		}
		return "", fmt.Errorf("manifest launcher version '%s', expected %s", got, launcher)
	}
	var names []string
	for _, want := range []string{"windows-x64-installer", "windows-x64-portable", "linux-deb", "linux-appimage"} {
		if _, ok := m.Launcher.Assets[want]; !ok {
			return "", errors.New("the manifest is missing launcher packages")
		}
	}
	for k := range m.Launcher.Assets {
		names = append(names, k)
	}
	sort.Strings(names)
	pb, err := os.ReadFile(filepath.Join(dir, "assets.json"))
	if err != nil {
		return "", err
	}
	var pub struct {
		Assets []asset `json:"assets"`
	}
	if err := json.Unmarshal(pb, &pub); err != nil {
		return "", errors.New("assets.json is not readable")
	}
	published := map[string]int64{}
	for _, a := range pub.Assets {
		published[a.Name] = a.Size
	}
	files := []asset{m.Shim.asset}
	for _, k := range names {
		files = append(files, m.Launcher.Assets[k])
	}
	for _, a := range files {
		if size, ok := published[a.Name]; !ok || size != a.Size || a.Size <= 0 {
			return "", fmt.Errorf("missing/incomplete release asset: %s", a.Name)
		}
	}
	return "manifest ok: " + strings.Join(names, ", "), nil
}

func checkGameDB(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	if len(data) == 0 || len(data) > 64<<20 || !bytes.HasPrefix(data, []byte("SQLite format 3\x00")) {
		return "", errors.New("invalid canonical database")
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	db, err := sql.Open("sqlite", "file:"+filepath.ToSlash(abs)+"?mode=ro")
	if err != nil {
		return "", err
	}
	defer db.Close()
	var ok string
	if err := db.QueryRow("PRAGMA quick_check").Scan(&ok); err != nil || ok != "ok" {
		return "", errors.New("database quick_check failed")
	}
	var cars int64
	if err := db.QueryRow("SELECT COUNT(*) FROM Data_Car").Scan(&cars); err != nil {
		return "", fmt.Errorf("Data_Car: %v", err)
	}
	if cars <= 0 {
		return "", errors.New("empty car catalog")
	}
	sum := sha256.Sum256(data)
	return fmt.Sprintf("canonical database: %d bytes, %d cars, SHA256 %s", len(data), cars, hex.EncodeToString(sum[:])), nil
}

var (
	azVersionRe = regexp.MustCompile(`(?m)^AZIMUTH_VERSION\s*=\s*"([^"]+)"`)
	azBuildsRe  = regexp.MustCompile(`(?s)^BUILTIN_AZIMUTH_BUILDS\s*=\s*\[(.*?)\n\]`)
	azEntryRe   = regexp.MustCompile(`\{\s*"version"\s*:\s*(AZIMUTH_VERSION|"[^"]+")\s*,\s*"sha256"\s*:\s*"([0-9a-f]{64})"\s*\}`)
)

type build struct {
	Version string `json:"version"`
	SHA256  string `json:"sha256"`
}

// compat reads AZIMUTH_VERSION and BUILTIN_AZIMUTH_BUILDS from the launcher's constants. Every entry of the list must be a {"version", "sha256"}
// pair (the version a literal or AZIMUTH_VERSION): anything else is refused rather than left out.
func compat(dir string) (string, error) {
	c, err := readConstants(dir)
	if err != nil {
		return "", err
	}
	v := azVersionRe.FindStringSubmatch(c)
	if v == nil {
		return "", errors.New("AZIMUTH_VERSION not found in the launcher's constants")
	}
	start := strings.Index(c, "\nBUILTIN_AZIMUTH_BUILDS")
	if start < 0 {
		return "", errors.New("BUILTIN_AZIMUTH_BUILDS not found in the launcher's constants")
	}
	b := azBuildsRe.FindStringSubmatch(c[start+1:])
	if b == nil {
		return "", errors.New("BUILTIN_AZIMUTH_BUILDS is not a list this tool can read")
	}
	entries := azEntryRe.FindAllStringSubmatch(b[1], -1)
	if len(entries) == 0 || len(entries) != strings.Count(b[1], "{") {
		return "", errors.New("BUILTIN_AZIMUTH_BUILDS has entries other than {\"version\", \"sha256\"}")
	}
	var builds []build
	for _, e := range entries {
		ver := v[1]
		if e[1] != "AZIMUTH_VERSION" {
			ver = strings.Trim(e[1], `"`)
		}
		builds = append(builds, build{ver, e[2]})
	}
	j, err := json.Marshal(struct {
		Latest    string  `json:"latestAzimuth"`
		Supported []build `json:"supportedAzimuth"`
	}{v[1], builds})
	if err != nil {
		return "", err
	}
	return base64.StdEncoding.EncodeToString(j), nil
}

func readObject(r io.Reader) (map[string]any, error) {
	var m map[string]any
	if err := json.NewDecoder(r).Decode(&m); err != nil || m == nil {
		return nil, errors.New("the input is not a JSON object")
	}
	return m, nil
}

func get(r io.Reader, def string, keys []string) (string, error) {
	m, err := readObject(r)
	if err != nil {
		return "", err
	}
	var out []string
	for _, k := range keys {
		switch v := m[k].(type) {
		case nil:
			out = append(out, def)
		case string:
			if v == "" {
				v = def
			}
			out = append(out, v)
		default:
			j, _ := json.Marshal(v)
			out = append(out, string(j))
		}
	}
	return strings.Join(out, " "), nil
}

func configCheck(r io.Reader, minLauncher, minShim, compatB64 string) (string, error) {
	c, err := readObject(r)
	if err != nil {
		return "", err
	}
	raw, err := base64.StdEncoding.DecodeString(compatB64)
	if err != nil {
		return "", errors.New("the compatibility metadata is not base64")
	}
	var want map[string]any
	if err := json.Unmarshal(raw, &want); err != nil {
		return "", errors.New("the compatibility metadata is not a JSON object")
	}
	for k, v := range want {
		if !reflect.DeepEqual(c[k], v) {
			return "", errors.New("server reports stale Azimuth compatibility metadata")
		}
	}
	gotL, _ := c["minLauncher"].(string)
	gotS, _ := c["minShim"].(string)
	line := fmt.Sprintf("server reports minLauncher/minShim: ('%s', '%s')", gotL, gotS)
	if gotL != minLauncher || gotS != minShim {
		return line, fmt.Errorf("the server does not report %s / %s: a setting saved through the operator API (PUT /ops/v1/config) overrides the file; set it there too", minLauncher, minShim)
	}
	return line, nil
}
