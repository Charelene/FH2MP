package crashreports

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"sort"
	"strings"
	"testing"
	"time"

	"fh2mp/internal/auth"
	"fh2mp/internal/pyjson"
)

func load(t *testing.T) pyjson.Obj {
	f, err := os.Open("../../testdata/crash.json.gz")
	if err != nil {
		t.Skip(err)
	}
	defer f.Close()
	zr, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	b, err := io.ReadAll(zr)
	if err != nil {
		t.Fatal(err)
	}
	v, err := pyjson.Parse(string(b))
	if err != nil {
		t.Fatal(err)
	}
	return v.(pyjson.Obj)
}

func list(o pyjson.Obj, k string) []any { v, _ := o.Get(k); l, _ := v.([]any); return l }
func str(v any) string                  { s, _ := v.(string); return s }

func TestRedact(t *testing.T) {
	fx := load(t)
	bad := 0
	for _, e := range list(fx, "redact") {
		p := e.([]any)
		if got := Redact(str(p[0])); got != str(p[1]) {
			bad++
			if bad <= 8 {
				t.Errorf("Redact(%q)\n got %q\nwant %q", str(p[0]), got, str(p[1]))
			}
		}
	}
	if bad > 0 {
		t.Errorf("%d redactions differ", bad)
	}
}

func TestSmallFunctions(t *testing.T) {
	fx := load(t)
	for _, e := range list(fx, "clean_line") {
		p := e.([]any)
		if got := CleanLine(p[0], 200); got != str(p[1]) {
			t.Errorf("CleanLine(%v) = %q, python %q", p[0], got, str(p[1]))
		}
		if got := CleanLine(p[0], 10); got != str(p[2]) {
			t.Errorf("CleanLine(%v, 10) = %q, python %q", p[0], got, str(p[2]))
		}
	}
	for _, e := range list(fx, "file_name") {
		p := e.([]any)
		if FileName(str(p[0]), false) != p[1].(bool) || FileName(str(p[0]), true) != p[2].(bool) {
			t.Errorf("FileName(%q) = %v %v, python %v %v", str(p[0]), FileName(str(p[0]), false), FileName(str(p[0]), true), p[1], p[2])
		}
	}
	for _, e := range list(fx, "decode_dump") {
		p := e.([]any)
		want := p[1].(pyjson.Obj)
		got, err := DecodeDump(p[0])
		if ok, has := want.Get("ok"); has {
			if err != nil || hex.EncodeToString(got) != str(ok) {
				t.Errorf("DecodeDump(%v) = %x %v, python ok %s", p[0], got, err, str(ok))
			}
			continue
		}
		er, _ := want.Get("err")
		triple := er.([]any)
		var ae *auth.Error
		if !errors.As(err, &ae) || int64(ae.Status) != triple[0].(int64) || ae.Code != str(triple[1]) || ae.Message != str(triple[2]) {
			t.Errorf("DecodeDump(%v): %v, python %v", p[0], err, triple)
		}
	}
	for _, e := range list(fx, "signature") {
		p := e.([]any)
		k, title := Signature(str(p[0]))
		w := p[1].([]any)
		if k != str(w[0]) || title != str(w[1]) {
			t.Errorf("Signature(%q) = %q %q, python %v", str(p[0]), k, title, w)
		}
	}
	for _, e := range list(fx, "failure") {
		p := e.([]any)
		line := FailureLine(str(p[0]))
		if line != str(p[1]) {
			t.Errorf("FailureLine(%q) = %q, python %q", str(p[0]), line, str(p[1]))
			continue
		}
		k, title := Signature(line)
		w := p[2].([]any)
		if k != str(w[0]) || title != str(w[1]) {
			t.Errorf("Signature(failure %q) = %q %q, python %v", line, k, title, w)
		}
	}
}

func bigPayload(name string, text any) string {
	b64 := func(b []byte) string { return base64.StdEncoding.EncodeToString(b) }
	f := `{"date": "2026-11-03", "system": "Windows 11", "gpu": "RTX 3080 Ti", "driver": "31.0.15.4601", "launcher": "1.1.5", "azimuth": "1.0.4", "extraOptions": "0.4.4", "name": "Maple"}`
	switch name {
	case "file_too_big":
		return `{"v": 1, "fields": ` + f + `, "files": {"x.log": "` + strings.Repeat("x", 2<<20+1) + `"}}`
	case "files_total_too_big":
		var parts []string
		for i := 0; i < 5; i++ {
			parts = append(parts, fmt.Sprintf(`"f%d.log": "%s"`, i, strings.Repeat("x", 2<<20)))
		}
		return `{"v": 1, "fields": ` + f + `, "files": {` + strings.Join(parts, ", ") + `}}`
	case "dump_total_too_big":
		d := b64(append([]byte("MDMP"), make([]byte, 20<<20)...))
		return `{"v": 1, "fields": ` + f + `, "files": {"azimuth.log": "x"}, "dump": "` + d + `", "attachments": {"a.dmp": "` + d + `"}}`
	}
	return ""
}

func parseReport(t *testing.T, payload string) (*Report, error) {
	v, err := pyjson.Parse(payload)
	if err != nil {
		t.Fatal(err)
	}
	return Validate(v)
}

func TestValidate(t *testing.T) {
	fx := load(t)
	for _, e := range list(fx, "validate") {
		o := e.(pyjson.Obj)
		name := str(mustGet(o, "name"))
		payload, _ := o.Get("payload")
		text := str(payload)
		if payload == nil {
			text = bigPayload(name, nil)
		}
		r, err := parseReport(t, text)
		if ok, has := o.Get("ok"); has {
			if err != nil {
				t.Errorf("%s: %v, python accepted it", name, err)
				continue
			}
			want := ok.(pyjson.Obj)
			// fields and files in order
			wf := mustGet(want, "fields").([]any)
			for i, k := range FieldKeys {
				if wf[i].([]any)[0] != k || wf[i].([]any)[1] != mustGet(pyjson.Obj{{K: "v", V: r.field(k)}}, "v") {
					t.Errorf("%s: field %s = %q, python %v", name, k, r.field(k), wf[i])
				}
			}
			wfiles := mustGet(want, "files").([]any)
			if len(wfiles) != len(r.Files) {
				t.Errorf("%s: %d files, python %d", name, len(r.Files), len(wfiles))
			} else {
				for i, p := range r.Files {
					if p.K != str(wfiles[i].([]any)[0]) || p.V != wfiles[i].([]any)[1] {
						t.Errorf("%s: file %d differs", name, i)
					}
				}
			}
			wd := mustGet(want, "dump")
			if (wd == nil) == r.HasDump || (r.HasDump && hex.EncodeToString(r.Dump) != str(wd)) {
				t.Errorf("%s: dump differs", name)
			}
			watt := mustGet(want, "attachments").([]any)
			if len(watt) != len(r.Attachments) {
				t.Errorf("%s: %d attachments, python %d", name, len(r.Attachments), len(watt))
			} else {
				for i, p := range r.Attachments {
					if p.K != str(watt[i].([]any)[0]) || hex.EncodeToString(p.V.([]byte)) != str(watt[i].([]any)[1]) {
						t.Errorf("%s: attachment %d differs", name, i)
					}
				}
			}
			if r.DumpStatus != str(mustGet(want, "dumpStatus")) {
				t.Errorf("%s: dumpStatus %q, python %q", name, r.DumpStatus, str(mustGet(want, "dumpStatus")))
			}
			continue
		}
		er := mustGet(o, "err").([]any)
		var ae *auth.Error
		if !errors.As(err, &ae) || int64(ae.Status) != er[0].(int64) || ae.Code != str(er[1]) || ae.Message != str(er[2]) {
			t.Errorf("%s: %v, python error %v", name, err, er)
		}
	}
}

func mustGet(o pyjson.Obj, k string) any {
	v, ok := o.Get(k)
	if !ok {
		panic("missing " + k)
	}
	return v
}

func TestTexts(t *testing.T) {
	fx := load(t)
	payloads := map[string]string{}
	for _, e := range list(fx, "validate") {
		o := e.(pyjson.Obj)
		if p, _ := o.Get("payload"); p != nil {
			payloads[str(mustGet(o, "name"))] = str(p)
		}
	}
	const rid = "0123456789abcdef"
	for _, e := range list(fx, "texts") {
		o := e.(pyjson.Obj)
		name := str(mustGet(o, "name"))
		text, ok := payloads[name]
		id := rid
		if p, has := o.Get("payload"); has {
			text, ok, id = str(p), true, strings.Repeat("a", 16)
		}
		if !ok {
			t.Fatalf("no payload for %s", name)
		}
		r, err := parseReport(t, text)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		line := FailureLine(r.file("azimuth.log"))
		check := func(what, got, want string) {
			if got != want {
				g, w := got, want
				if len(g) > 600 {
					g = g[:600]
				}
				if len(w) > 600 {
					w = w[:600]
				}
				t.Errorf("%s %s differs\n got %q\nwant %q", name, what, g, w)
			}
		}
		check("line", line, str(mustGet(o, "line")))
		check("summary", SummaryTable(id, r, line), str(mustGet(o, "summary")))
		check("links", FileLinks(id, r, LinkBase), str(mustGet(o, "links")))
		if lc := str(mustGet(o, "links_custom")); lc != "" {
			check("links_custom", FileLinks(id, r, "https://x.test/p/"), lc)
			check("comment_custom", CommentBody(id, r, "https://x.test/p/"), str(mustGet(o, "comment_custom")))
		}
		check("issue", IssueBody(id, r, line, LinkBase), str(mustGet(o, "issue")))
		check("comment", CommentBody(id, r, LinkBase), str(mustGet(o, "comment")))
	}
}

// ---- the report directory with a fake GitHub

type scripted struct {
	err  string
	body any
}

type recorded struct {
	Method, URL string
	Data        any
	Headers     [][2]string
}

func listing(t *testing.T, base string) any {
	var rows [][]any
	filepath.Walk(base, func(p string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() {
			return nil
		}
		rel, _ := filepath.Rel(base, p)
		rel = filepath.ToSlash(rel)
		data, _ := os.ReadFile(p)
		if strings.HasSuffix(rel, ".dmp") {
			sum := sha256.Sum256(data)
			rows = append(rows, []any{rel, "bin", hex.EncodeToString(sum[:]), int64(len(data))})
		} else {
			rows = append(rows, []any{rel, "text", strings.ReplaceAll(string(data), "\r\n", "\n")})
		}
		return nil
	})
	sort.Slice(rows, func(i, j int) bool { return rows[i][0].(string) < rows[j][0].(string) })
	out := make([]any, len(rows))
	for i, r := range rows {
		out[i] = r
	}
	return out
}

func normLog(lines []string) []any {
	re1 := regexp.MustCompile(`unreadable \([^)]*\)`)
	re2 := regexp.MustCompile(`could not store it: .*`)
	re3 := regexp.MustCompile(`crash report index .*? unreadable`)
	out := []any{}
	for _, l := range lines {
		l = re1.ReplaceAllString(l, "unreadable (...)")
		l = re2.ReplaceAllString(l, "could not store it: ...")
		l = re3.ReplaceAllString(l, "crash report index <path> unreadable")
		out = append(out, l)
	}
	return out
}

func normLogAny(l []any) []any {
	var s []string
	for _, x := range l {
		s = append(s, str(x))
	}
	return normLog(s)
}

func TestDirectoryMatchesPython(t *testing.T) {
	fx := load(t)
	steps := list(fx, "dir")
	payloads := map[string]string{}
	for _, e := range list(fx, "validate") {
		o := e.(pyjson.Obj)
		if p, _ := o.Get("payload"); p != nil {
			payloads[str(mustGet(o, "name"))] = str(p)
		}
	}
	base := filepath.Join(t.TempDir(), "reports")
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	must(os.MkdirAll(base, 0o755))
	clock := 1_700_000_000.0
	counter := 0
	token := func() string { counter++; return fmt.Sprintf("%016x", counter) }
	old := filepath.Join(base, "00000000000000aa")
	must(os.MkdirAll(old, 0o755))
	must(os.WriteFile(filepath.Join(old, "x.log"), []byte("old"), 0o644))
	at := func(p string, days float64) {
		ts := time.Unix(0, int64((clock-days*86400)*1e9))
		must(os.Chtimes(p, ts, ts))
	}
	at(old, 100)
	keep := filepath.Join(base, "00000000000000bb")
	must(os.MkdirAll(keep, 0o755))
	at(keep, 10)
	other := filepath.Join(base, "not-an-id")
	must(os.MkdirAll(other, 0o755))
	at(other, 200)

	var logs []string
	store := New(base, nil, func(s string) { logs = append(logs, s) }, false, LinkBase)
	store.Now = func() float64 { return clock }
	store.Token = token
	var requests []recorded
	var script []scripted
	gh := &GitHub{Repo: "owner/repo", Token: "tok123", Open: func(req *http.Request) (*http.Response, error) {
		var data any
		if req.Body != nil {
			b, _ := io.ReadAll(req.Body)
			data = string(b)
		}
		var hs [][2]string
		for k, v := range req.Header {
			hs = append(hs, [2]string{strings.ToLower(k), v[0]})
		}
		sort.Slice(hs, func(i, j int) bool { return hs[i][0] < hs[j][0] })
		requests = append(requests, recorded{req.Method, req.URL.String(), data, hs})
		s := script[0]
		script = script[1:]
		if s.err != "" {
			return nil, errors.New(s.err)
		}
		body, _ := pyjson.Dumps(s.body, pyjson.Opts{})
		return &http.Response{StatusCode: 200, Body: io.NopCloser(strings.NewReader(body))}, nil
	}}
	n := 0
	run := func(name string, fn func() (any, error), withRequests bool) {
		t.Helper()
		if n >= len(steps) {
			t.Fatalf("%s: no more steps", name)
		}
		want := steps[n].(pyjson.Obj)
		n++
		if str(mustGet(want, "s")) != name {
			t.Fatalf("step %d is %s in Python and %s in Go", n, mustGet(want, "s"), name)
		}
		got, err := fn()
		if er, has := want.Get("err"); has {
			var ae *auth.Error
			triple := er.([]any)
			if !errors.As(err, &ae) || int64(ae.Status) != triple[0].(int64) || ae.Code != str(triple[1]) || ae.Message != str(triple[2]) {
				t.Fatalf("%s: %v, python error %v", name, err, triple)
			}
		} else {
			if err != nil {
				t.Fatalf("%s: %v", name, err)
			}
			w, _ := want.Get("r")
			if ws, ok := w.(string); ok { // Python wrote these text files on Windows, with CRLF line ends
				w = strings.ReplaceAll(ws, "\r\n", "\n")
			}
			if !reflect.DeepEqual(norm(got), norm(w)) {
				t.Fatalf("%s: result %v, python %v", name, got, w)
			}
		}
		wl, _ := want.Get("log")
		if gl, wl := normLog(logs), normLogAny(wl.([]any)); !reflect.DeepEqual(gl, wl) {
			t.Fatalf("%s: log\n got %q\nwant %q", name, gl, wl)
		}
		logs = nil
		wls, _ := want.Get("ls")
		if gl := listing(t, base); !reflect.DeepEqual(norm(gl), norm(wls)) {
			gb, _ := pyjson.Dumps(gl, pyjson.Opts{})
			wb, _ := pyjson.Dumps(wls, pyjson.Opts{})
			t.Fatalf("%s: directory\n got %.1500s\nwant %.1500s", name, gb, wb)
		}
		if withRequests {
			wr, _ := want.Get("requests")
			var gr []any
			for _, r := range requests {
				hs := []any{}
				for _, h := range r.Headers {
					hs = append(hs, []any{h[0], h[1]})
				}
				gr = append(gr, pyjson.Obj{{K: "method", V: r.Method}, {K: "url", V: r.URL}, {K: "data", V: r.Data}, {K: "headers", V: hs}})
			}
			var wr2 []any
			for _, r := range wr.([]any) {
				o := r.(pyjson.Obj)
				var hs []any
				for _, h := range mustGet(o, "headers").([]any) {
					hs = append(hs, h)
				}
				wr2 = append(wr2, pyjson.Obj{{K: "method", V: mustGet(o, "method")}, {K: "url", V: mustGet(o, "url")}, {K: "data", V: mustGet(o, "data")}, {K: "headers", V: hs}})
			}
			if !reflect.DeepEqual(norm(gr), norm(wr2)) {
				gb, _ := pyjson.Dumps(gr, pyjson.Opts{})
				wb, _ := pyjson.Dumps(wr2, pyjson.Opts{})
				t.Fatalf("%s: requests\n got %.2500s\nwant %.2500s", name, gb, wb)
			}
		}
		requests = nil
	}
	submit := func(s *Reports, payload string) func() (any, error) {
		return func() (any, error) {
			v, err := pyjson.Parse(payload)
			if err != nil {
				return nil, err
			}
			rid, err := s.Submit(v)
			if err != nil {
				return nil, err
			}
			return rid, nil
		}
	}
	clock = 1_700_000_100
	run("submit_minimal", submit(store, payloads["minimal"]), false)
	clock = 1_700_000_200
	run("submit_full", submit(store, payloads["full"]), false)
	run("submit_bad", func() (any, error) { _, err := store.Submit(pyjson.Obj{{K: "v", V: int64(3)}}); return nil, err }, false)
	dl := func(rid, name string, hexOut bool) func() (any, error) {
		return func() (any, error) {
			b, err := store.Download(rid, name)
			if err != nil {
				return nil, err
			}
			if hexOut {
				return hex.EncodeToString(b), nil
			}
			return string(b), nil
		}
	}
	run("download_log", dl("0000000000000001", "azimuth.log", false), false)
	run("download_dmp", dl("0000000000000002", "crash.dmp", true), false)
	run("download_att", dl("0000000000000002", "mod.dmp", true), false)
	for _, c := range [][2]string{{"0000000000000002", "missing.log"}, {"../x", "azimuth.log"}, {"0000000000000001", "../azimuth.log"}, {"ZZZZ", "a.log"}, {"0000000000000001", "report.json"}, {"0000000000000009", "azimuth.log"}} {
		run("download_"+c[0]+"_"+c[1], dl(c[0], c[1], false), false)
	}
	counter = 2
	run("submit_storage_failed", submit(store, payloads["minimal"]), false)

	store2 := New(base, gh, func(s string) { logs = append(logs, s) }, false, "https://x.test/panel")
	store2.Now = func() float64 { return clock }
	store2.Token = token
	counter = 10
	clock = 1_700_000_300
	script = []scripted{{body: pyjson.Obj{{K: "number", V: int64(41)}}}}
	run("gh_create", submit(store2, payloads["full"]), true)
	script = []scripted{{body: pyjson.Obj{{K: "state", V: "open"}}}, {body: pyjson.Obj{}}}
	run("gh_comment_known", submit(store2, payloads["full"]), true)
	script = []scripted{{body: pyjson.Obj{{K: "state", V: "closed"}}}, {body: pyjson.Obj{{K: "number", V: int64(42)}}}}
	run("gh_closed_new", submit(store2, payloads["full"]), true)
	nokey := `{"v": 1, "fields": ` + `{"date": "2026-11-03", "system": "Windows 11", "gpu": "RTX 3080 Ti", "driver": "31.0.15.4601", "launcher": "1.1.5", "azimuth": "1.0.4", "extraOptions": "0.4.4", "name": "Maple"}` + `, "files": {"azimuth.log": "nothing useful here"}}`
	script = []scripted{{body: pyjson.Obj{{K: "number", V: int64(43)}}}}
	run("gh_no_signature", submit(store2, nokey), true)
	script = []scripted{{body: pyjson.Obj{{K: "number", V: int64(44)}}}}
	run("gh_no_signature_again", submit(store2, nokey), true)
	stop := strings.Replace(nokey, "nothing useful here", "5 tid=1 STOPPED waiting 0xdead at azimuth.exe+0x20 (modified: no)", 1)
	script = []scripted{{body: pyjson.Obj{{K: "number", V: int64(45)}}}}
	run("gh_stop_signature", submit(store2, stop), true)
	script = []scripted{{err: "no route"}}
	run("gh_down", submit(store2, stop), true)
	script = []scripted{{body: pyjson.Obj{{K: "state", V: "open"}}}, {err: "boom"}}
	run("gh_comment_fails", submit(store2, stop), true)
	script = []scripted{{body: pyjson.Obj{{K: "state", V: "closed"}}}, {body: pyjson.Obj{{K: "oops", V: int64(1)}}}}
	run("gh_no_number", submit(store2, payloads["minimal"]), true)
	must(os.WriteFile(filepath.Join(base, "issues.json"), []byte("not json"), 0o644))
	script = []scripted{{body: pyjson.Obj{{K: "number", V: int64(50)}}}}
	run("gh_index_corrupt", submit(store2, stop), true)
	if n != len(steps) {
		t.Fatalf("Go ran %d steps, Python %d", n, len(steps))
	}
	_ = bytes.MinRead
}

// norm makes two JSON trees comparable (ints and floats of equal value are the same number).
func norm(v any) any {
	switch x := v.(type) {
	case pyjson.Obj:
		out := make(map[string]any, len(x))
		for _, p := range x {
			out[p.K] = norm(p.V)
		}
		return out
	case []any:
		out := make([]any, len(x))
		for i, e := range x {
			out[i] = norm(e)
		}
		return out
	case [][2]string:
		out := make([]any, len(x))
		for i, e := range x {
			out[i] = []any{e[0], e[1]}
		}
		return out
	case int64:
		return float64(x)
	case int:
		return float64(x)
	case uint64:
		return float64(x)
	}
	return v
}
