// Package diag is the shared logging layer of both servers (= server/diag.py): levels, per-subsystem verbosity, rate limiting, key=value
// formatting and secret scrubbing. Line shape: `HH:MM:SS [LEVEL ]<context> <message>`; INFO carries no level word.
package diag

import (
	"fmt"
	"os"
	"regexp"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/pyfmt"
)

// Levels.
const (
	Trace = 5
	Debug = 10
	Info  = 20
	Warn  = 30
	Error = 40
)

var levels = map[string]int{"TRACE": Trace, "DEBUG": Debug, "INFO": Info, "WARN": Warn, "ERROR": Error}

// LevelName returns the upper-case name of a level number.
func LevelName(l int) string {
	for k, v := range levels {
		if v == l {
			return k
		}
	}
	return "INFO"
}

func parseSubs(text string) map[string]int {
	out := map[string]int{}
	for _, item := range strings.Split(text, ",") {
		name, level, _ := strings.Cut(item, "=")
		name, level = strings.TrimSpace(name), strings.ToUpper(strings.TrimSpace(level))
		if l, ok := levels[level]; ok && name != "" {
			out[name] = l
		}
	}
	return out
}

// Verbosity says which lines are written; read once from the environment, tests pass their own.
type Verbosity struct {
	Base int
	Subs map[string]int
}

// NewVerbosity reads FH2MP_LOG_LEVEL / FH2MP_LOG_SUB unless level or subs are given (nil = environment).
func NewVerbosity(level, subs *string) Verbosity {
	l := os.Getenv("FH2MP_LOG_LEVEL")
	if l == "" {
		l = "INFO"
	}
	if level != nil {
		l = *level
	}
	s := os.Getenv("FH2MP_LOG_SUB")
	if subs != nil {
		s = *subs
	}
	base, ok := levels[strings.ToUpper(l)]
	if !ok {
		base = Info
	}
	return Verbosity{base, parseSubs(s)}
}

// Enabled reports whether a line of this level (for this subsystem, "" = none) is written.
func (v Verbosity) Enabled(level int, sub string) bool {
	if sub != "" {
		if l, ok := v.Subs[sub]; ok {
			return level >= l
		}
	}
	return level >= v.Base
}

// RateLimit allows at most one line per key per interval; the next line that gets through says how many it replaced.
type RateLimit struct {
	mu    sync.Mutex
	clock func() float64
	limit int
	state map[any]rateState
}

type rateState struct {
	next    float64
	skipped int
}

var start = time.Now()

func monotonic() float64 { return time.Since(start).Seconds() }

// NewRateLimit makes a limiter; clock nil = monotonic seconds, limit 0 = 2048 keys.
func NewRateLimit(clock func() float64, limit int) *RateLimit {
	if clock == nil {
		clock = monotonic
	}
	if limit == 0 {
		limit = 2048
	}
	return &RateLimit{clock: clock, limit: limit, state: map[any]rateState{}}
}

// Allow returns (write?, suppressed since the last written line).
func (r *RateLimit) Allow(key any, interval float64) (bool, int) {
	now := r.clock()
	r.mu.Lock()
	defer r.mu.Unlock()
	st := r.state[key]
	if now < st.next {
		st.skipped++
		r.state[key] = st
		return false, 0
	}
	if len(r.state) >= r.limit {
		r.state = map[any]rateState{}
	}
	r.state[key] = rateState{now + interval, 0}
	return true, st.skipped
}

var secrets = regexp.MustCompile(`(?i)(fh2\.[A-Za-z0-9_\-]{8,}|Bearer\s+\S+|X-FH2MP-Token\S*\s*[:=]\s*\S+|(?:token|secret|password|private[_ ]?key)\s*[=:]\s*\S+)`)

// Scrub removes session tokens, bearer headers and key=value secrets.
func Scrub(text string) string { return secrets.ReplaceAllString(text, "<redacted>") }

var runID = regexp.MustCompile(`^[0-9a-f]{8}(?:\.[0-9]{1,10})?$`)

// RunID returns text when it is a run id (`ab12cd34` or `ab12cd34.57`), else "".
func RunID(text string) string {
	if text != "" && runID.MatchString(text) {
		return text
	}
	return ""
}

var lost = NewRateLimit(nil, 0)

// WriteLine writes a log line; a failure goes to stderr at most once a minute with the number of lines lost.
func WriteLine(write func(string) error, line string) {
	if err := write(line); err != nil {
		if ok, skipped := lost.Allow("lost", 60.0); ok {
			fmt.Fprintf(os.Stderr, "log.write_failed %s\n", KV(KVP{"error", fmt.Sprintf("%T: %v", err, err)}, KVP{"lost", skipped + 1}))
		}
	}
}

// KVP is one key=value field; a nil Value is left out.
type KVP struct {
	K string
	V any
}

// KV formats `a=1 b='two words'` in the order given; nil fields are left out.
func KV(fields ...KVP) string {
	parts := make([]string, 0, len(fields))
	for _, f := range fields {
		if f.V == nil {
			continue
		}
		s := fmt.Sprint(f.V)
		switch v := f.V.(type) {
		case float64:
			s = pyfmt.ReprFloat(v)
		case bool:
			if v {
				s = "True"
			} else {
				s = "False"
			}
		}
		if s == "" || strings.ContainsAny(s, " \t'\"") {
			parts = append(parts, f.K+"="+pyfmt.ReprStr(s))
		} else {
			parts = append(parts, f.K+"="+s)
		}
	}
	return strings.Join(parts, " ")
}

var (
	sinkMu sync.RWMutex
	sink   func(level int, sub, msg string)
)

// SetSink points every logger at fn(level, sub, message); the game server and the LSP call this once at start.
func SetSink(fn func(level int, sub, msg string)) {
	sinkMu.Lock()
	sink = fn
	sinkMu.Unlock()
}

// Default is the verbosity used until a sink exists.
var Default = NewVerbosity(nil, nil)

// Logger writes lines of one subsystem.
type Logger struct{ Sub string }

// Get returns the logger of a subsystem.
func Get(sub string) *Logger { return &Logger{sub} }

func (l *Logger) emit(level int, msg string) {
	sinkMu.RLock()
	fn := sink
	sinkMu.RUnlock()
	if fn != nil {
		fn(level, l.Sub, msg)
		return
	}
	if Default.Enabled(level, l.Sub) {
		word := ""
		if level != Info {
			word = LevelName(level) + " "
		}
		fmt.Printf("%s %s%s\n", time.Now().Format("15:04:05"), word, Scrub(msg))
	}
}

// Trace, Debug, Info, Warn and Error write `<sub>: <msg>`.
func (l *Logger) Trace(msg string) { l.emit(Trace, l.Sub+": "+msg) }
func (l *Logger) Debug(msg string) { l.emit(Debug, l.Sub+": "+msg) }
func (l *Logger) Info(msg string)  { l.emit(Info, l.Sub+": "+msg) }
func (l *Logger) Warn(msg string)  { l.emit(Warn, l.Sub+": "+msg) }
func (l *Logger) Error(msg string) { l.emit(Error, l.Sub+": "+msg) }

// Exc logs a caught error: what was being done, then the error text; level is ERROR unless given.
func (l *Logger) Exc(msg string, err error, level ...int) {
	lv := Error
	if len(level) > 0 {
		lv = level[0]
	}
	l.emit(lv, fmt.Sprintf("%s: %s: %s", l.Sub, msg, Scrub(fmt.Sprintf("%T: %v", err, err))))
}
