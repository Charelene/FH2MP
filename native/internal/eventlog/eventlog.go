// Package eventlog writes events.jsonl, one JSON object per line (= server/event_log.py). Writing never fails into the game server.
package eventlog

import (
	"fmt"
	"math"
	"os"
	"sync"
	"time"

	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
)

var (
	log  = diag.Get("events")
	rate = diag.NewRateLimit(nil, 0)
)

// Log is an open events.jsonl; a nil *Log or a Log without a file ignores Emit.
type Log struct {
	Path string
	mu   sync.Mutex
	f    *os.File
	// Now is the wall clock in seconds, replaced by tests.
	Now func() float64
}

// Open opens path for appending; a file that cannot be opened is said in the log and the Log stays silent.
func Open(path string) *Log {
	l := &Log{Path: path, Now: func() float64 { return float64(time.Now().UnixNano()) / 1e9 }}
	if path != "" {
		f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		if err != nil {
			log.Error("events.open_failed " + diag.KV(diag.KVP{K: "path", V: path}, diag.KVP{K: "error", V: errText(err)},
				diag.KVP{K: "effect", V: "no events.jsonl records this run"}))
		} else {
			l.f = f
		}
	}
	return l
}

func errText(err error) string { return fmt.Sprintf("%T: %v", err, err) }

// Emit writes {"t": ..., "kind": kind, ...fields} on one line.
func (l *Log) Emit(kind string, fields ...pyjson.Pair) {
	if l == nil || l.f == nil {
		return
	}
	obj := pyjson.Obj{{K: "t", V: math.Round(l.Now()*10) / 10}, {K: "kind", V: kind}}
	for _, p := range fields {
		obj.Set(p.K, p.V)
	}
	line, err := pyjson.Dumps(obj, pyjson.Opts{Compact: true, DefaultStr: true})
	if err != nil {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if _, err := l.f.WriteString(line + "\n"); err != nil {
		if ok, skipped := rate.Allow("write", 60.0); ok {
			log.Error("events.write_failed " + diag.KV(diag.KVP{K: "path", V: l.Path}, diag.KVP{K: "kind", V: kind},
				diag.KVP{K: "lost", V: skipped + 1}, diag.KVP{K: "error", V: errText(err)}))
		}
	}
}

// Close closes the file.
func (l *Log) Close() {
	if l != nil && l.f != nil {
		l.f.Close()
	}
}
