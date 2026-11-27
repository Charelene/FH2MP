// Package gamedbsync serves the car database every online player runs with (= server/gamedb_sync.py): the operator's gamedbRC.slt, xz-compressed,
// re-read when it changes.
package gamedbsync

import (
	"bytes"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"

	"github.com/ulikunitz/xz"
	_ "modernc.org/sqlite" // pure-Go SQLite driver ("sqlite"): the add-on copies are built with it

	"fh2mp/internal/addons"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyfmt"
)

var log = diag.Get("gamedb")

// Constants.
const (
	FileName     = "gamedbRC.slt"
	DownloadPath = "/v1/gamedb/" + FileName + ".xz"
	MaxSize      = 64 * 1024 * 1024
)

// Manifest is what launchers compare with their copy.
type Manifest struct {
	Name   string
	Sha256 string
	Size   int
	XzSize int
	Path   string
}

// GameDb serves the file.
type GameDb struct {
	Path    string
	mu      sync.Mutex
	key     [2]int64
	haveKey bool
	problem string
	sha     string
	size    int
	blob    []byte
	// variants: the file with an add-on's rows (addons.LightbarsStatements), for the players whose games have that add-on's parts. Built
	// from the file each time it changes; an add-on whose copy could not be built has none (its players get the plain file).
	variants map[string]*variant
}

type variant struct {
	sha  string
	size int
	blob []byte
}

// variantSQL: the add-ons that get a copy of the file, and their statements.
var variantSQL = map[string][]string{addons.Lightbars: addons.LightbarsStatements}

// VariantPath is where an add-on's copy is downloaded.
func VariantPath(name string) string { return "/v1/gamedb/" + name + "/" + FileName + ".xz" }

// VariantManifestPath is where an add-on's copy is described.
func VariantManifestPath(name string) string { return "/v1/gamedb/" + name }

// buildVariant runs statements on a copy of base (a temporary file, removed after) -> the copy's bytes and the rows the statements added.
func buildVariant(base []byte, statements []string) ([]byte, int64, error) {
	f, err := os.CreateTemp("", "fh2mp-gamedb-*.slt")
	if err != nil {
		return nil, 0, err
	}
	path := f.Name()
	defer func() {
		os.Remove(path)
		os.Remove(path + "-journal")
	}()
	if _, err := f.Write(base); err != nil {
		f.Close()
		return nil, 0, err
	}
	if err := f.Close(); err != nil {
		return nil, 0, err
	}
	db, err := sql.Open("sqlite", "file:"+strings.ReplaceAll(path, "\\", "/"))
	if err != nil {
		return nil, 0, err
	}
	db.SetMaxOpenConns(1)
	var rows int64
	err = func() error {
		tx, err := db.Begin()
		if err != nil {
			return err
		}
		for _, s := range statements {
			res, err := tx.Exec(s)
			if err != nil {
				tx.Rollback()
				return fmt.Errorf("%v (statement %s...)", err, pyfmt.ReprStr(s[:min(len(s), 60)]))
			}
			n, _ := res.RowsAffected()
			rows += n
		}
		return tx.Commit()
	}()
	if cerr := db.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return nil, 0, err
	}
	data, err := os.ReadFile(path)
	return data, rows, err
}

// buildVariants makes every add-on's copy of data (the file just read); logs each.
func (g *GameDb) buildVariants(data []byte) {
	out := map[string]*variant{}
	for name, statements := range variantSQL {
		copyData, rows, err := buildVariant(data, statements)
		if err != nil {
			log.Warn(fmt.Sprintf("car database copy for add-on %s could not be built: %v; its players get the plain database (their cars' add-on parts are not in it)", name, err))
			continue
		}
		sum := sha256.Sum256(copyData)
		var buf bytes.Buffer
		w, _ := xz.NewWriter(&buf)
		w.Write(copyData)
		w.Close()
		v := &variant{hex.EncodeToString(sum[:]), len(copyData), buf.Bytes()}
		out[name] = v
		log.Info(fmt.Sprintf("car database copy for add-on %s now served: sha256 %s %d bytes (%d compressed), %d rows added to %s", name, v.sha[:12], v.size, len(v.blob), rows, g.sha[:12]))
	}
	g.variants = out
}

// New serves path ("" = FH2MP_SYNC_GAMEDB).
func New(path string) *GameDb {
	if path == "" {
		path = os.Getenv("FH2MP_SYNC_GAMEDB")
	}
	return &GameDb{Path: path}
}

func (g *GameDb) report(key, msg string) {
	if g.problem != key {
		g.problem = key
		if len(g.blob) > 0 {
			msg += "; the last good copy " + g.sha[:12] + " keeps being served"
		} else {
			msg += "; there is no copy to serve, launchers get 404"
		}
		log.Warn(msg)
	}
}

// load brings the copy up to date with the file; true when there is something to serve.
func (g *GameDb) load() bool {
	if g.Path == "" {
		return false
	}
	st, err := os.Stat(g.Path)
	if err != nil {
		g.report("os:"+err.Error(), fmt.Sprintf("car database %s cannot be read: %v", g.Path, err))
		return len(g.blob) > 0
	}
	key := [2]int64{st.ModTime().UnixNano(), st.Size()}
	same := g.haveKey && key == g.key
	if !same && st.Size() > MaxSize {
		g.report(fmt.Sprintf("big:%v", key), fmt.Sprintf("car database %s is %d bytes, above the %d byte limit", g.Path, st.Size(), MaxSize))
	}
	if !same && st.Size() <= MaxSize {
		f, err := os.Open(g.Path)
		if err != nil {
			g.report("os:"+err.Error(), fmt.Sprintf("car database %s cannot be read: %v", g.Path, err))
			return len(g.blob) > 0
		}
		data, err := io.ReadAll(io.LimitReader(f, MaxSize+1))
		f.Close()
		if err != nil {
			g.report("os:"+err.Error(), fmt.Sprintf("car database %s cannot be read: %v", g.Path, err))
			return len(g.blob) > 0
		}
		if len(data) >= 16 && string(data[:16]) == "SQLite format 3\x00" && len(data) <= MaxSize {
			old := g.sha
			sum := sha256.Sum256(data)
			g.sha, g.size = hex.EncodeToString(sum[:]), len(data)
			var buf bytes.Buffer
			w, _ := xz.NewWriter(&buf)
			w.Write(data)
			w.Close()
			g.blob = buf.Bytes()
			g.key, g.haveKey = key, true
			g.problem = ""
			extra := ""
			if old != "" && old != g.sha {
				extra = ", replaces " + old[:12]
			}
			log.Info(fmt.Sprintf("car database now served: sha256 %s %d bytes (%d compressed)%s", g.sha[:12], g.size, len(g.blob), extra))
			g.buildVariants(data)
		} else {
			head := data
			if len(head) > 16 {
				head = head[:16]
			}
			g.report(fmt.Sprintf("format:%v", key), fmt.Sprintf("car database %s (%d bytes) does not start with the SQLite header, it starts with b%s", g.Path, len(data), pyfmt.ReprStr(string(head))))
		}
	}
	return len(g.blob) > 0
}

// Manifest returns the manifest, or nil when no database is set.
func (g *GameDb) Manifest() *Manifest {
	g.mu.Lock()
	defer g.mu.Unlock()
	if !g.load() {
		return nil
	}
	return &Manifest{FileName, g.sha, g.size, len(g.blob), DownloadPath}
}

// VariantManifest returns add-on name's copy, nil when there is none (no database set, or the copy could not be built).
func (g *GameDb) VariantManifest(name string) *Manifest {
	g.mu.Lock()
	defer g.mu.Unlock()
	if !g.load() {
		return nil
	}
	v := g.variants[name]
	if v == nil {
		return nil
	}
	return &Manifest{FileName, v.sha, v.size, len(v.blob), VariantPath(name)}
}

// VariantDownload returns add-on name's copy, xz-compressed, nil when there is none.
func (g *GameDb) VariantDownload(name string) []byte {
	g.mu.Lock()
	defer g.mu.Unlock()
	if !g.load() {
		return nil
	}
	if v := g.variants[name]; v != nil {
		return v.blob
	}
	return nil
}

// VariantOf is the add-on whose copy has sha256 sha ("" for none).
func (g *GameDb) VariantOf(sha string) string {
	g.mu.Lock()
	defer g.mu.Unlock()
	if !g.load() {
		return ""
	}
	for name, v := range g.variants {
		if v.sha == sha {
			return name
		}
	}
	return ""
}

// Download returns the xz-compressed file, or nil when no database is set.
func (g *GameDb) Download() []byte {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.load() {
		return g.blob
	}
	return nil
}
