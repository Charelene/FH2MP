// Package crashreports takes the crash reports of the launcher (POST /v1/crash-reports): kept on disk and turned into a GitHub issue
// (= server/crash_reports.py). The issue is public, so it carries only the summary, the Mod Platform's crash report and the end of azimuth.log, each
// passed through Redact; fh2mp.log, the platform and launcher logs and the crash dump stay in <dir>/<id>/. A crash that was reported before (same
// exception and address) is added to the open issue as a comment instead of a new issue; if that issue was closed, a new one is opened.
package crashreports

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/dlclark/regexp2"

	"fh2mp/internal/auth"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
)

// Limits and constants.
const (
	MaxBody   = 96 << 20 // 32 MiB of dumps (base64) + 8 MiB of text, including worst-case JSON escaping
	MaxText   = 8 << 20
	MaxFile   = 2 << 20
	MaxFiles  = 64
	MaxDump   = 32 << 20 // combined Azimuth and Mod Platform dumps
	LinkBase  = "https://forza.scuffed.af/panel/crashes"
	KeepDays  = 90
	BodyLimit = 60_000 // GitHub refuses issue bodies over 65536 characters
)

// FieldKeys are the report fields in their order, with the labels the issue shows.
var FieldKeys = []string{"date", "system", "gpu", "driver", "launcher", "azimuth", "extraOptions", "name"}

var fieldLabels = map[string]string{"date": "Date", "system": "System", "gpu": "GPU", "driver": "Driver's version", "launcher": "Launcher version",
	"azimuth": "Azimuth version", "extraOptions": "FH2 Extra Options version", "name": "Your FH2MP name"}

// IssueLabels: not "bug": that label is for what players report themselves, so the two kinds can be told apart.
var IssueLabels = []string{"crash-report"}

var (
	fileRe  = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.-]{0,119}\.(?:log|txt|dmp)\z`)
	idRe    = regexp.MustCompile(`^[0-9a-f]{16}$`)
	b64Re   = regexp.MustCompile(`^[A-Za-z0-9+/]*={0,2}$`)
	extraRe = regexp.MustCompile(`^extra-[0-9a-f]{10}-`)
)

func rx(p string) *regexp2.Regexp { return regexp2.MustCompile(p, regexp2.None) }

const address = `(?:[A-Za-z0-9_.-]+\+)?0x[0-9a-fA-F]+`

var (
	failRe    = rx(`\b(CRASHED|STOPPED)\b`)
	sessionRe = rx(`\bsession \S+ began `)
	codeRe    = rx(`CRASHED exception (0x[0-9a-fA-F]+) at (` + address + `)`)
	stopRe    = rx(`STOPPED (.*?) at (` + address + `)`)
	infinity  = rx(`(?i)^infinity(?:[.-].*)?\.log\z`)
)

// Secrets and personal data that may be in a log line: e-mail addresses, IP addresses (not loopback; dotted version numbers such as a driver or game
// build are left alone), credentials in URLs, Authorization / cookie headers, key=value secrets and long base64 runs (keys, seeds).
var (
	ip4Re = rx(`(?<![\w.])(?:25[0-5]|2[0-4]\d|1?\d?\d)(?:\.(?:25[0-5]|2[0-4]\d|1?\d?\d)){3}(?![\w.])`)
	tokRe = rx(`\bfh2\.[A-Za-z0-9_.\-]{16,}`)
	b64Rn = rx(`(?<![A-Za-z0-9+/_-])[A-Za-z0-9+/_-]{40,}={0,2}`)
)

type rule struct {
	re   *regexp2.Regexp
	repl func(m *regexp2.Match, runes []rune) string
}

func group(m *regexp2.Match, i int) string { return m.Groups()[i].String() }

var versionWords = []string{"version", "driver", "build", "ver", "v"}

var secretRules = []rule{
	{rx(`(?i)\b([a-z][a-z0-9+.-]*://)[^/\s:@]+:[^/\s@]+@`), func(m *regexp2.Match, _ []rune) string { return group(m, 1) + "<credentials>@" }},
	{rx(`[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}`), func(*regexp2.Match, []rune) string { return "<email>" }},
	{rx(`(?i)\b(authorization|proxy-authorization|x-fh2mp-token|cookie|set-cookie)(\s*[:=]\s*)[^\r\n]+`), func(m *regexp2.Match, _ []rune) string { return group(m, 1) + group(m, 2) + "<secret>" }},
	{rx(`(?i)\b(bearer|basic|xbl3\.0)\s+[^\s"']{8,}`), func(m *regexp2.Match, _ []rune) string { return group(m, 1) + " <secret>" }},
	{rx(`(?i)\b(recovery[_-]?\w*|mnemonic|seed[ _-]?phrase)(\s*[=:]\s*)[^\r\n]+`), func(m *regexp2.Match, _ []rune) string { return group(m, 1) + group(m, 2) + "<secret>" }},
	{rx(`(?i)\b(password|passwd|pwd|secret|token|api[_-]?key|private[_-]?key|seed)(\s*[=:]\s*)("[^"]*"|'[^']*'|[^\s,;]+)`), func(m *regexp2.Match, _ []rune) string { return group(m, 1) + group(m, 2) + "<secret>" }},
	{ip4Re, func(m *regexp2.Match, runes []rune) string {
		ip := m.String()
		start := m.Index
		from := max(0, start-12)
		before := strings.ToLower(string(runes[from:start]))
		before = strings.TrimRight(before, " =:")
		if strings.HasPrefix(ip, "127.") || strings.HasPrefix(ip, "0.") || strings.HasSuffix(ip, ".0") || ip == "1.0.0.64" {
			return ip
		}
		for _, w := range versionWords {
			if strings.HasSuffix(before, w) {
				return ip
			}
		}
		return "<ip>"
	}},
	{rx(`(?i)(?<![\w:])(?:[23][0-9a-f]{3}|f[cd][0-9a-f]{2}|fe80)(?::[0-9a-f]{0,4}){2,7}(?![\w:])`), func(*regexp2.Match, []rune) string { return "<ip>" }},
	{b64Rn, func(m *regexp2.Match, _ []rune) string {
		s := m.String()
		if strings.Count(s, "/") >= 2 || !(strings.ContainsAny(s, "abcdefghijklmnopqrstuvwxyz") && strings.ContainsAny(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") && strings.ContainsAny(s, "0123456789")) {
			return s // a path, a hex digest or a word: not a key
		}
		return "<secret>"
	}},
}

func replaceAll(re *regexp2.Regexp, text string, f func(m *regexp2.Match, runes []rune) string) string {
	runes := []rune(text)
	out, err := re.ReplaceFunc(text, func(m regexp2.Match) string { return f(&m, runes) }, -1, -1)
	if err != nil {
		return text
	}
	return out
}

// Redact blanks the secret patterns, and session tokens (fh2.*), for anything that goes into a public issue.
func Redact(text string) string {
	text = replaceAll(tokRe, text, func(*regexp2.Match, []rune) string { return "<token>" })
	for _, r := range secretRules {
		text = replaceAll(r.re, text, r.repl)
	}
	return text
}

// PyStr is str(v) for a JSON value.
func PyStr(v any) string {
	switch x := v.(type) {
	case nil:
		return "None"
	case string:
		return x
	case bool:
		if x {
			return "True"
		}
		return "False"
	case int64:
		return strconv.FormatInt(x, 10)
	case uint64:
		return strconv.FormatUint(x, 10)
	case float64:
		return pyfmt.ReprFloat(x)
	case []any:
		parts := make([]string, len(x))
		for i, e := range x {
			parts[i] = pyRepr(e)
		}
		return "[" + strings.Join(parts, ", ") + "]"
	case pyjson.Obj:
		parts := make([]string, len(x))
		for i, p := range x {
			parts[i] = pyfmt.ReprStr(p.K) + ": " + pyRepr(p.V)
		}
		return "{" + strings.Join(parts, ", ") + "}"
	}
	return fmt.Sprint(v)
}

func pyRepr(v any) string {
	if s, ok := v.(string); ok {
		return pyfmt.ReprStr(s)
	}
	return PyStr(v)
}

var ctlRe = regexp.MustCompile(`[\x00-\x1f\x7f]+`)

func cut(s string, n int) string {
	if utf8.RuneCountInString(s) <= n {
		return s
	}
	return string([]rune(s)[:n])
}

// pyStrip is str.strip(): Unicode whitespace both ends.
func pyStrip(s string) string {
	return strings.TrimFunc(s, func(r rune) bool { return isPySpace(r) })
}

func isPySpace(r rune) bool {
	switch r {
	case ' ', '\t', '\n', '\r', '\v', '\f', 0x1c, 0x1d, 0x1e, 0x1f, 0x85, 0xa0, 0x1680, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000:
		return true
	}
	return r >= 0x2000 && r <= 0x200a
}

// CleanLine is one printable line: control characters (newlines too) become spaces.
func CleanLine(v any, limit int) string {
	return cut(pyStrip(ctlRe.ReplaceAllString(PyStr(v), " ")), limit)
}

// FileName says whether name is an allowed attachment name (binary: a .dmp).
func FileName(name string, binary bool) bool {
	return fileRe.MatchString(name) && !strings.Contains(name, "..") && strings.HasSuffix(name, ".dmp") == binary
}

// DecodeDump checks and decodes a base64 minidump.
func DecodeDump(value any) ([]byte, error) {
	s, ok := value.(string)
	if !ok || len(s) > 4*((MaxDump+2)/3) {
		return nil, auth.NewError(413, "too_large", "the crash dump is too big")
	}
	if !b64Re.MatchString(s) {
		return nil, auth.NewError(400, "bad_dump", "the crash dump is not valid base64")
	}
	data, err := base64.StdEncoding.DecodeString(s)
	if err != nil {
		return nil, auth.NewError(400, "bad_dump", "the crash dump is not valid base64")
	}
	if len(data) > MaxDump {
		return nil, auth.NewError(413, "too_large", "the crash dump is too big")
	}
	if !bytes.HasPrefix(data, []byte("MDMP")) {
		return nil, auth.NewError(400, "bad_dump", "that is not a minidump")
	}
	return data, nil
}

// Report is a validated crash report.
type Report struct {
	Fields      pyjson.Obj // FieldKeys order
	Files       pyjson.Obj // name -> text, in the order sent
	Dump        []byte
	HasDump     bool
	Attachments pyjson.Obj // name -> []byte
	DumpStatus  string
}

func (r *Report) field(k string) string { v, _ := r.Fields.Get(k); s, _ := v.(string); return s }

func (r *Report) file(name string) string { v, _ := r.Files.Get(name); s, _ := v.(string); return s }

func isOne(v any) bool {
	switch x := v.(type) {
	case int64:
		return x == 1
	case uint64:
		return x == 1
	case float64:
		return x == 1
	case bool:
		return x
	}
	return false
}

func truthy(v any) bool {
	switch x := v.(type) {
	case nil:
		return false
	case string:
		return x != ""
	case bool:
		return x
	case int64:
		return x != 0
	case uint64:
		return x != 0
	case float64:
		return x != 0
	case []any:
		return len(x) > 0
	case pyjson.Obj:
		return len(x) > 0
	}
	return true
}

// Validate checks a report; anything else is an auth.Error (400 / 413).
func Validate(d any) (*Report, error) {
	obj, ok := d.(pyjson.Obj)
	if !ok {
		return nil, auth.NewError(400, "bad_request", "unknown report format")
	}
	if v, _ := obj.Get("v"); !isOne(v) {
		return nil, auth.NewError(400, "bad_request", "unknown report format")
	}
	fv, _ := obj.Get("fields")
	f, ok := fv.(pyjson.Obj)
	if !ok {
		return nil, auth.NewError(400, "bad_request", "no fields")
	}
	r := &Report{}
	for _, k := range FieldKeys {
		v, has := f.Get(k)
		if !has {
			v = ""
		}
		r.Fields = append(r.Fields, pyjson.Pair{K: k, V: CleanLine(v, 200)})
	}
	rf, _ := obj.Get("files")
	ra, _ := obj.Get("attachments")
	var rawFiles, rawAtt pyjson.Obj
	if truthy(rf) {
		o, ok := rf.(pyjson.Obj)
		if !ok {
			return nil, auth.NewError(400, "bad_request", "invalid file list")
		}
		rawFiles = o
	}
	if truthy(ra) {
		o, ok := ra.(pyjson.Obj)
		if !ok {
			return nil, auth.NewError(400, "bad_request", "invalid file list")
		}
		rawAtt = o
	}
	if len(rawFiles)+len(rawAtt) > MaxFiles {
		return nil, auth.NewError(400, "bad_request", "invalid file list")
	}
	totalText := 0
	for _, p := range rawFiles {
		text, isStr := p.V.(string)
		if !FileName(p.K, false) || !isStr {
			return nil, auth.NewError(400, "bad_request", "unknown file")
		}
		totalText += len(text)
		if len(text) > MaxFile || totalText > MaxText {
			return nil, auth.NewError(413, "too_large", p.K+" is too big")
		}
		r.Files.Set(p.K, text)
	}
	if dv, _ := obj.Get("dump"); truthy(dv) {
		dump, err := DecodeDump(dv)
		if err != nil {
			return nil, err
		}
		r.Dump, r.HasDump = dump, true
	}
	totalDump := len(r.Dump)
	for _, p := range rawAtt {
		if !FileName(p.K, true) || p.K == "crash.dmp" {
			return nil, auth.NewError(400, "bad_request", "invalid attachment name")
		}
		data, err := DecodeDump(p.V)
		if err != nil {
			return nil, err
		}
		r.Attachments.Set(p.K, data)
		totalDump += len(data)
		if totalDump > MaxDump {
			return nil, auth.NewError(413, "too_large", "the crash dumps are too big")
		}
	}
	anyFile := false
	for _, p := range r.Files {
		if s, _ := p.V.(string); s != "" {
			anyFile = true
		}
	}
	if !anyFile && !r.HasDump && len(r.Attachments) == 0 {
		return nil, auth.NewError(400, "bad_request", "empty report")
	}
	ds, _ := obj.Get("dumpStatus")
	if ds == nil {
		ds = ""
	}
	r.DumpStatus = CleanLine(ds, 200)
	return r, nil
}

// SplitLines is str.splitlines().
func SplitLines(s string) []string {
	var out []string
	start := 0
	rs := []rune(s)
	for i := 0; i < len(rs); i++ {
		switch rs[i] {
		case '\n', '\v', '\f', 0x1c, 0x1d, 0x1e, 0x85, 0x2028, 0x2029:
			out = append(out, string(rs[start:i]))
			start = i + 1
		case '\r':
			out = append(out, string(rs[start:i]))
			if i+1 < len(rs) && rs[i+1] == '\n' {
				i++
			}
			start = i + 1
		}
	}
	if start < len(rs) {
		out = append(out, string(rs[start:]))
	}
	return out
}

func lastN(l []string, n int) []string {
	if n >= len(l) {
		return l
	}
	return l[len(l)-n:]
}

// FailureLine is the first failure in the most recent failing session, before failures in crash handling itself. A subsequent launcher-only session
// may already have started when the report is submitted: keep the previous failure in that case, but do not let a secondary Wine/dbghelp or Azimuth
// worker exception replace its cause.
func FailureLine(azimuthLog string) string {
	failure, first := "", ""
	for _, ln := range SplitLines(azimuthLog) {
		if ok, _ := sessionRe.MatchString(ln); ok {
			first = ""
		}
		if first == "" {
			if f, _ := failRe.MatchString(ln); f && !strings.Contains(ln, "CreateDXGIFactory2") {
				first, failure = ln, ln
			}
		}
	}
	if failure == "" {
		return ""
	}
	ln, _ := tidRe.Replace(failure, "", -1, -1)
	ln, _, _ = strings.Cut(ln, " (modified:")
	return CleanLine(ln, 300)
}

var (
	nonWordRe = rx(`[^\w .,:+()/-]`)
	hexRe     = rx(`0x[0-9a-fA-F]+`)
	tidRe     = rx(`^\d+ tid=\d+ `)
)

// Signature names a crash by its exception and address, so the same crash from different players lands in one issue: (key, title).
func Signature(line string) (string, string) {
	if m, _ := codeRe.FindStringMatch(line); m != nil {
		return strings.ToLower(group(m, 1)) + "@" + strings.ToLower(group(m, 2)), "Crash " + group(m, 1) + " at " + group(m, 2)
	}
	if m, _ := stopRe.FindStringMatch(line); m != nil {
		hexed, _ := hexRe.Replace(group(m, 1), "0x", -1, -1)
		plain, _ := nonWordRe.Replace(hexed, "", -1, -1)
		what := pyStrip(cut(plain, 70))
		return "stop:" + what + "@" + strings.ToLower(group(m, 2)), "Azimuth stop at " + group(m, 2) + ": " + what
	}
	return "", "Game crash (no failure line in azimuth.log)"
}

// code is inline code that cannot break out of its table cell or ping anyone.
func code(v string) string {
	return "`" + strings.ReplaceAll(strings.ReplaceAll(CleanLine(v, 200), "`", "'"), "|", "/") + "`"
}

var tickRe = regexp.MustCompile("`+")

func fence(text string) string {
	longest := 0
	for _, m := range tickRe.FindAllString(text, -1) {
		longest = max(longest, len(m))
	}
	return strings.Repeat("`", max(3, longest+1))
}

func tailBlock(title, text string, lines, budget int) string {
	text = strings.Join(lastN(SplitLines(text), lines), "\n")
	if rs := []rune(text); len(rs) > budget {
		tail := string(rs[len(rs)-budget:])
		if _, after, ok := strings.Cut(tail, "\n"); ok {
			tail = after
		}
		text = tail
	}
	if pyStrip(text) == "" {
		return ""
	}
	f := fence(text)
	return "<details><summary>" + title + "</summary>\n\n" + f + "text\n" + text + "\n" + f + "\n\n</details>\n"
}

// SummaryTable is the table at the top of an issue.
func SummaryTable(rid string, r *Report, line string) string {
	type row struct{ k, v string }
	crash := Redact(line)
	if crash == "" {
		crash = "not found in azimuth.log"
	}
	rows := []row{{"Crash", crash}}
	for _, k := range FieldKeys {
		rows = append(rows, row{fieldLabels[k], Redact(r.field(k))})
	}
	rows = append(rows, row{"Report id", rid})
	var b strings.Builder
	b.WriteString("| | |\n|-|-|\n")
	for _, x := range rows {
		v := "-"
		if x.v != "" {
			v = code(x.v)
		}
		b.WriteString("| " + x.k + " | " + v + " |\n")
	}
	return b.String()
}

func splitExt(p string) (string, string) {
	i := strings.LastIndex(p, ".")
	if i <= 0 || strings.Trim(p[:i], ".") == "" {
		return p, ""
	}
	return p[:i], p[i:]
}

// FileLinks lists the download links of a report's attachments.
func FileLinks(rid string, r *Report, linkBase string) string {
	var names []string
	for _, p := range r.Files {
		if s, _ := p.V.(string); s != "" {
			names = append(names, p.K)
		}
	}
	if r.HasDump {
		names = append(names, "crash.dmp")
	}
	for _, p := range r.Attachments {
		names = append(names, p.K)
	}
	sort.Strings(names)
	labels := map[string]bool{}
	var links []string
	for _, name := range names {
		base := extraRe.ReplaceAllString(name, "") // older launchers prefixed every additional log with a path hash
		if ok, _ := infinity.MatchString(base); ok {
			continue
		}
		label := base
		stem, ext := splitExt(base)
		for n := 2; labels[strings.ToLower(label)]; n++ {
			label = fmt.Sprintf("%s-%d%s", stem, n, ext)
		}
		labels[strings.ToLower(label)] = true
		links = append(links, fmt.Sprintf("- [%s](%s/%s/%s/download/)", label, strings.TrimRight(linkBase, "/"), rid, name))
	}
	return strings.Join(links, "\n")
}

// IssueBody is the text of a new issue.
func IssueBody(rid string, r *Report, line, linkBase string) string {
	head := "This issue was created automatically by launcher's crash reporter.\n\n" + SummaryTable(rid, r, line)
	var blocks []string
	for _, b := range []string{tailBlock("FH2 Mod Platform crash report", Redact(r.file("mod-crash.txt")), 80, 8_000), tailBlock("azimuth.log, last 40 lines", Redact(r.file("azimuth.log")), 40, 10_000)} {
		if b != "" {
			blocks = append(blocks, b)
		}
	}
	return cut(head+"\n\n"+FileLinks(rid, r, linkBase)+"\n\n"+strings.Join(blocks, "\n"), BodyLimit)
}

// CommentBody is the text added to the open issue of a known crash.
func CommentBody(rid string, r *Report, linkBase string) string {
	var b strings.Builder
	b.WriteString("Another report of this crash.\n\n| | |\n|-|-|\n")
	for _, k := range FieldKeys {
		v := "-"
		if f := r.field(k); f != "" {
			v = code(Redact(f))
		}
		b.WriteString("| " + fieldLabels[k] + " | " + v + " |\n")
	}
	b.WriteString("| Report id | `" + rid + "` |\n\n" + FileLinks(rid, r, linkBase))
	return b.String()
}

// ---------------------------------------------------------------- GitHub

// Opener sends a request (http.Client.Do; tests replace it).
type Opener func(req *http.Request) (*http.Response, error)

// GitHub is the three issue calls this needs.
type GitHub struct {
	Repo, Token string
	Open        Opener
}

func (g *GitHub) call(method, path string, body pyjson.Obj) (pyjson.Obj, error) {
	var data io.Reader
	if body != nil {
		text, err := pyjson.Dumps(body, pyjson.Opts{})
		if err != nil {
			return nil, err
		}
		data = strings.NewReader(text)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, method, "https://api.github.com/repos/"+g.Repo+path, data)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+g.Token)
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("X-GitHub-Api-Version", "2022-11-28")
	req.Header.Set("User-Agent", "fh2mp-server")
	open := g.Open
	if open == nil {
		open = http.DefaultClient.Do
	}
	resp, err := open(req)
	if err != nil {
		return nil, fmt.Errorf("URLError: <urlopen error %v>", err)
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode >= 400 {
		return nil, fmt.Errorf("HTTPError: HTTP Error %d: %s", resp.StatusCode, http.StatusText(resp.StatusCode))
	}
	if len(b) == 0 {
		return pyjson.Obj{}, nil
	}
	v, err := pyjson.Parse(string(b))
	if err != nil {
		return nil, fmt.Errorf("JSONDecodeError: %v", err)
	}
	o, _ := v.(pyjson.Obj)
	return o, nil
}

// IssueState is the state ("open", "closed") of an issue.
func (g *GitHub) IssueState(number int64) (string, error) {
	o, err := g.call("GET", fmt.Sprintf("/issues/%d", number), nil)
	if err != nil {
		return "", err
	}
	v, _ := o.Get("state")
	s, _ := v.(string)
	return s, nil
}

// CreateIssue opens an issue and returns its number.
func (g *GitHub) CreateIssue(title, body string) (int64, error) {
	labels := make([]any, len(IssueLabels))
	for i, l := range IssueLabels {
		labels[i] = l
	}
	o, err := g.call("POST", "/issues", pyjson.Obj{{K: "title", V: title}, {K: "body", V: body}, {K: "labels", V: labels}})
	if err != nil {
		return 0, err
	}
	v, ok := o.Get("number")
	if !ok {
		return 0, fmt.Errorf("KeyError: 'number'")
	}
	return asInt(v), nil
}

// Comment adds a comment to an issue.
func (g *GitHub) Comment(number int64, body string) error {
	_, err := g.call("POST", fmt.Sprintf("/issues/%d/comments", number), pyjson.Obj{{K: "body", V: body}})
	return err
}

func asInt(v any) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case float64:
		return int64(x)
	}
	return 0
}

// Reports keeps and publishes crash reports.
type Reports struct {
	Dir        string
	GitHub     *GitHub
	Log        func(string)
	Background bool
	LinkBase   string
	// Now and Token make the receive time and the report id (tests replace them).
	Now   func() float64
	Token func() string
	mu    sync.Mutex
}

// FromEnv builds the store from FH2MP_CRASH_DIR / FH2MP_GITHUB_TOKEN / FH2MP_GITHUB_REPO / FH2MP_CRASH_LINK_BASE; nil when neither dir nor token is set.
func FromEnv(logf func(string)) *Reports {
	dir, token := os.Getenv("FH2MP_CRASH_DIR"), os.Getenv("FH2MP_GITHUB_TOKEN")
	if dir == "" && token == "" {
		return nil
	}
	var gh *GitHub
	if token != "" {
		repo := os.Getenv("FH2MP_GITHUB_REPO")
		if repo == "" {
			repo = "Charelene/FH2MP"
		}
		gh = &GitHub{Repo: repo, Token: token}
	}
	lb := os.Getenv("FH2MP_CRASH_LINK_BASE")
	if lb == "" {
		lb = LinkBase
	}
	return New(dir, gh, logf, true, lb)
}

// New makes a store.
func New(dir string, gh *GitHub, logf func(string), background bool, linkBase string) *Reports {
	if logf == nil {
		logf = func(string) {}
	}
	return &Reports{Dir: dir, GitHub: gh, Log: logf, Background: background, LinkBase: linkBase,
		Now: func() float64 { return float64(time.Now().UnixNano()) / 1e9 },
		Token: func() string {
			b := make([]byte, 8)
			rand.Read(b)
			return hex.EncodeToString(b)
		}}
}

// Submit checks and keeps one report and starts the GitHub part. Returns the report id.
func (c *Reports) Submit(payload any) (string, error) {
	r, err := Validate(payload)
	if err != nil {
		return "", err
	}
	rid := c.Token()
	line := FailureLine(r.file("azimuth.log"))
	if c.Dir != "" {
		if err := c.store(rid, r, line); err != nil {
			return "", err
		}
	}
	if c.GitHub != nil {
		if c.Background {
			go c.Publish(rid, r, line)
		} else {
			c.Publish(rid, r, line)
		}
	}
	c.Log("  crash report " + rid + " " + pyfmt.ReprStr(cut(line, 100)))
	return rid, nil
}

func (c *Reports) store(rid string, r *Report, line string) error {
	d := filepath.Join(c.Dir, rid)
	fail := func(err error) error {
		c.Log(fmt.Sprintf("  crash report %s: could not store it: %v", rid, err))
		return auth.NewError(503, "storage_failed", "could not store the crash report")
	}
	if err := os.Mkdir(d, 0o700); err != nil {
		return fail(err)
	}
	for _, p := range r.Files {
		if err := os.WriteFile(filepath.Join(d, p.K), []byte(p.V.(string)), 0o644); err != nil {
			return fail(err)
		}
	}
	if r.HasDump {
		if err := os.WriteFile(filepath.Join(d, "crash.dmp"), r.Dump, 0o644); err != nil {
			return fail(err)
		}
	}
	for _, p := range r.Attachments {
		if err := os.WriteFile(filepath.Join(d, p.K), p.V.([]byte), 0o644); err != nil {
			return fail(err)
		}
	}
	meta := pyjson.Obj{{K: "received", V: int64(c.Now())}, {K: "fields", V: r.Fields}, {K: "failure", V: line}, {K: "issue", V: nil}, {K: "dumpStatus", V: r.DumpStatus}}
	if err := c.writeMeta(rid, meta); err != nil {
		return fail(err)
	}
	if err := c.prune(); err != nil {
		return fail(err)
	}
	return nil
}

// Download returns an attachment of a report.
func (c *Reports) Download(rid, name string) ([]byte, error) {
	notFound := auth.NewError(404, "not_found", "no such attachment")
	if c.Dir == "" || !idRe.MatchString(rid) || !(FileName(name, false) || FileName(name, true)) {
		return nil, notFound
	}
	folder := filepath.Join(c.Dir, rid)
	path := filepath.Join(folder, name)
	for _, p := range []string{folder, path} {
		if st, err := os.Lstat(p); err == nil && st.Mode()&os.ModeSymlink != 0 {
			return nil, notFound
		}
	}
	limit := MaxFile
	if strings.HasSuffix(name, ".dmp") {
		limit = MaxDump
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, notFound
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, int64(limit)+1))
	if err != nil {
		return nil, notFound
	}
	if len(data) > limit {
		return nil, auth.NewError(413, "too_large", "the attachment is too big")
	}
	return data, nil
}

func (c *Reports) writeMeta(rid string, meta pyjson.Obj) error {
	p := filepath.Join(c.Dir, rid, "report.json")
	text, err := pyjson.Dumps(meta, pyjson.Opts{Indent: 1})
	if err != nil {
		return err
	}
	if err := os.WriteFile(p+".tmp", []byte(text), 0o644); err != nil {
		return err
	}
	return os.Rename(p+".tmp", p)
}

func (c *Reports) prune() error {
	cutoff := time.Unix(0, int64((c.Now()-KeepDays*86400)*1e9))
	entries, err := os.ReadDir(c.Dir)
	if err != nil {
		return err
	}
	for _, e := range entries {
		p := filepath.Join(c.Dir, e.Name())
		if !idRe.MatchString(e.Name()) {
			continue
		}
		st, err := os.Stat(p)
		if err != nil || !st.IsDir() {
			continue
		}
		if st.ModTime().Before(cutoff) {
			os.RemoveAll(p)
		}
	}
	return nil
}

func (c *Reports) indexPath() string {
	if c.Dir == "" {
		return ""
	}
	return filepath.Join(c.Dir, "issues.json")
}

func (c *Reports) index() pyjson.Obj {
	b, err := os.ReadFile(c.indexPath())
	if err != nil {
		if !os.IsNotExist(err) {
			c.Log(fmt.Sprintf("  crash report index %s unreadable (OSError: %v); treated as empty, so a crash that has an open issue gets a new one", c.indexPath(), err))
		}
		return pyjson.Obj{}
	}
	v, err := pyjson.Parse(string(b))
	o, ok := v.(pyjson.Obj)
	if err != nil || !ok {
		c.Log(fmt.Sprintf("  crash report index %s unreadable (ValueError: %v); treated as empty, so a crash that has an open issue gets a new one", c.indexPath(), err))
		return pyjson.Obj{}
	}
	return o
}

// Publish comments on the open issue for this crash, or opens one. It never fails loudly: a failure is logged and the report stays on disk.
// Returns the issue number (0 = none).
func (c *Reports) Publish(rid string, r *Report, line string) int64 {
	key, title := Signature(line)
	var number int64
	err := func() error {
		c.mu.Lock() // two players crashing at once must not open two issues
		defer c.mu.Unlock()
		index := pyjson.Obj{}
		if c.Dir != "" {
			index = c.index()
		}
		if key != "" {
			if v, ok := index.Get(key); ok {
				number = asInt(v)
			}
		}
		if number != 0 {
			state, err := c.GitHub.IssueState(number)
			if err != nil {
				return err
			}
			if state == "open" {
				return c.GitHub.Comment(number, CommentBody(rid, r, c.LinkBase))
			}
		}
		n, err := c.GitHub.CreateIssue(title, IssueBody(rid, r, line, c.LinkBase))
		if err != nil {
			return err
		}
		number = n
		if key != "" && c.Dir != "" {
			index.Set(key, number)
			text, _ := pyjson.Dumps(index, pyjson.Opts{})
			if err := os.WriteFile(c.indexPath()+".tmp", []byte(text), 0o644); err != nil {
				return err
			}
			return os.Rename(c.indexPath()+".tmp", c.indexPath())
		}
		return nil
	}()
	if err != nil {
		c.Log(fmt.Sprintf("  crash report %s: GitHub failed: %v", rid, err))
		return 0
	}
	if c.Dir != "" {
		metaPath := filepath.Join(c.Dir, rid, "report.json")
		b, err := os.ReadFile(metaPath)
		var meta pyjson.Obj
		if err == nil {
			var v any
			if v, err = pyjson.Parse(string(b)); err == nil {
				meta, _ = v.(pyjson.Obj)
			}
		}
		if err == nil {
			meta.Set("issue", number)
			err = c.writeMeta(rid, meta)
		}
		if err != nil {
			c.Log(fmt.Sprintf("  crash report %s: issue #%d exists but report.json was not updated with it: %v", rid, number, err))
		}
	}
	c.Log(fmt.Sprintf("  crash report %s: issue #%d", rid, number))
	return number
}
