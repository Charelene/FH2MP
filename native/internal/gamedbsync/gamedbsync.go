// Package gamedbsync serves the car database every online player runs with (= server/gamedb_sync.py): the operator's gamedbRC.slt, xz-compressed,
// re-read when it changes.
package gamedbsync

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"sync"

	"github.com/ulikunitz/xz"

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

// Download returns the xz-compressed file, or nil when no database is set.
func (g *GameDb) Download() []byte {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.load() {
		return g.blob
	}
	return nil
}
