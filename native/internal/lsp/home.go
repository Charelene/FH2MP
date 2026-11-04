package lsp

import (
	"encoding/xml"
	"fmt"
	"html"
	"io"
	"net/http"
	"net/mail"
	"os"
	"regexp"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
)

// What the launcher's home page (and the website, if it likes) shows: GET /v1/home, public, one JSON document. GitHub and itch.io are asked by the server (at
// most hourly, again after a failure; the last good answer is kept), so players' launchers never call them. Built only from public data.

var homeLog = diag.Get("home")

const (
	feedTTL    = 3600.0
	feedRetry  = 600.0
	firstRetry = 60.0
	carDays    = 7
	itchFeed   = "https://almost-there-games.itch.io/azimuth/devlog.rss"
)

var updateRe = regexp.MustCompile(`(?i)^\s*(?:update|azimuth)\s+v?(\d+\.\d+\.\d+)\b`)

func githubRepo() string {
	if r := os.Getenv("FH2MP_GITHUB_REPO"); r != "" {
		return r
	}
	return "Charelene/FH2MP"
}

func httpGet(url string, limit int64, accept string) ([]byte, error) {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "fh2mp-server")
	if accept != "" {
		req.Header.Set("Accept", accept)
	}
	cl := &http.Client{Timeout: 6 * time.Second}
	resp, err := cl.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		return nil, fmt.Errorf("HTTP Error %d: %s", resp.StatusCode, resp.Status)
	}
	return io.ReadAll(io.LimitReader(resp.Body, limit))
}

var tagRe, wsRe = regexp.MustCompile(`<[^>]+>`), regexp.MustCompile(`\s+`)

// plain turns an HTML snippet into plain text.
func plain(fragment string, limit int) string {
	text := html.UnescapeString(tagRe.ReplaceAllString(fragment, " "))
	text = strings.TrimSpace(wsRe.ReplaceAllString(text, " "))
	r := []rune(text)
	if len(r) <= limit {
		return text
	}
	return strings.TrimRight(string(r[:limit-1]), " \t\n\r") + "…"
}

func parseRelease(raw []byte) (any, error) {
	v, err := pyjson.Parse(string(raw))
	if err != nil {
		return nil, err
	}
	for _, rv := range anyList(v) {
		r, _ := rv.(pyjson.Obj)
		if truthy(objAny(r, "draft")) {
			continue
		}
		s := func(k string) string {
			if v := objAny(r, k); truthy(v) {
				return pyjson.PyStr(v)
			}
			return ""
		}
		return pyjson.Obj{{K: "tag", V: s("tag_name")}, {K: "name", V: s("name")}, {K: "url", V: s("html_url")}, {K: "published", V: s("published_at")}, {K: "body", V: cut(s("body"), 20000)}}, nil
	}
	return nil, nil
}

type rssItem struct {
	Title       string `xml:"title"`
	Link        string `xml:"link"`
	PubDate     string `xml:"pubDate"`
	Description string `xml:"description"`
}

func parseDevlog(raw []byte) (any, error) {
	var feed struct {
		Items []rssItem `xml:"channel>item"`
	}
	if err := xml.Unmarshal(raw, &feed); err != nil {
		return nil, err
	}
	posts := []any{}
	var all []pyjson.Obj
	for _, it := range feed.Items {
		title, link := strings.TrimSpace(it.Title), strings.TrimSpace(it.Link)
		var date int64
		if t, err := mail.ParseDate(strings.TrimSpace(it.PubDate)); err == nil {
			date = t.Unix()
		}
		if title != "" && strings.HasPrefix(link, "https://") {
			all = append(all, pyjson.Obj{{K: "title", V: title}, {K: "url", V: link}, {K: "date", V: date}, {K: "summary", V: plain(it.Description, 400)}})
		}
	}
	latest := ""
	for _, p := range all {
		if m := updateRe.FindStringSubmatch(pyjson.PyStr(objAny(p, "title"))); m != nil {
			latest = m[1]
			break
		}
	}
	for i, p := range all {
		if i < 3 {
			posts = append(posts, p)
		}
	}
	return pyjson.Obj{{K: "latest", V: latest}, {K: "posts", V: posts}}, nil
}

// feeds are the two outside sources, each refreshed at most hourly. While one goroutine refreshes, the others serve the last good copy; before there is any
// copy they wait for that first fetch. A failed fetch is retried after feedRetry, or firstRetry while there is nothing yet.
type feeds struct {
	fetch func(url string, limit int64, accept string) ([]byte, error)
	clock func() float64
	mu    map[string]*sync.Mutex
	state sync.Mutex
	data  map[string]any
	due   map[string]float64
}

func newFeeds() *feeds {
	return &feeds{fetch: httpGet, clock: nowF, mu: map[string]*sync.Mutex{"release": {}, "azimuth": {}}, data: map[string]any{}, due: map[string]float64{}}
}

func (f *feeds) load(key string) (any, error) {
	if key == "release" {
		raw, err := f.fetch(fmt.Sprintf("https://api.github.com/repos/%s/releases?per_page=5", githubRepo()), 1_000_000, "application/vnd.github+json")
		if err != nil {
			return nil, err
		}
		return parseRelease(raw)
	}
	raw, err := f.fetch(itchFeed, 1_000_000, "")
	if err != nil {
		return nil, err
	}
	return parseDevlog(raw)
}

func (f *feeds) get(key string) any {
	f.state.Lock()
	if f.clock() < f.due[key] {
		d := f.data[key]
		f.state.Unlock()
		return d
	}
	cur := f.data[key]
	f.state.Unlock()
	lock := f.mu[key]
	if cur != nil {
		if !lock.TryLock() {
			return cur // someone else is refreshing it
		}
	} else {
		lock.Lock()
	}
	defer lock.Unlock()
	f.state.Lock()
	if f.clock() < f.due[key] { // it was fetched while this goroutine waited
		d := f.data[key]
		f.state.Unlock()
		return d
	}
	f.state.Unlock()
	v, err := f.load(key)
	f.state.Lock()
	defer f.state.Unlock()
	if err != nil { // an outside site being down must never break /v1/home
		retry := feedRetry
		if f.data[key] == nil {
			retry = firstRetry
		}
		f.due[key] = f.clock() + retry
		what := "nothing to serve yet"
		if f.data[key] != nil {
			what = "serving the copy from the last good fetch"
		}
		homeLog.Warn(fmt.Sprintf("%s feed fetch failed, %s, retry in %s s: %v", key, what, fmtG(retry), err))
		return f.data[key]
	}
	f.data[key] = v
	f.due[key] = f.clock() + feedTTL
	return v
}

var homeFeeds = newFeeds()

func (o *Ops) topCars(days, limit int) []any {
	names := must(o.C.Riv.Names()).Car
	rows := must(o.C.DB.Query("SELECT car, COUNT(*) AS n FROM score_submissions WHERE submitted > ? GROUP BY car ORDER BY n DESC, car LIMIT ?", nowF()-float64(days*86400), limit*2))
	out := []any{}
	for _, r := range rows {
		if n := names[r.Int("car")]; n != "" && len(out) < limit {
			out = append(out, pyjson.Obj{{K: "car", V: n}, {K: "runs", V: r.Int("n")}})
		}
	}
	return out
}

func (c *Core) buildHome() pyjson.Obj {
	o := c.opsAPI()
	cfg := c.Launcher.(*Launcher).Config.Get()
	ch := o.challenges(3)
	var recs []pyjson.Obj
	for _, ev := range anyList(objAny(ch, "events")) {
		e, _ := ev.(pyjson.Obj)
		rows := anyList(objAny(e, "rows"))
		if len(rows) == 0 {
			continue
		}
		r0, _ := rows[0].(pyjson.Obj)
		recs = append(recs, pyjson.Obj{{K: "gamertag", V: objAny(r0, "gamertag")}, {K: "xuid", V: objAny(r0, "xuid")}, {K: "event", V: objAny(e, "name")}, {K: "score", V: objAny(r0, "score")},
			{K: "car", V: objAny(r0, "car")}, {K: "class", V: objAny(r0, "class")}, {K: "at", V: objAny(r0, "at")}})
	}
	sortStable(recs, func(a, b pyjson.Obj) bool { return toInt(objAny(a, "at")) > toInt(objAny(b, "at")) })
	records := []any{}
	for i, r := range recs {
		if i < 3 {
			records = append(records, r)
		}
	}
	az := pyjson.Obj{{K: "latest", V: ""}, {K: "posts", V: []any{}}}
	if v, ok := homeFeeds.get("azimuth").(pyjson.Obj); ok {
		az = cloneObj(v)
	}
	if v, ok := cfg.Get("latestAzimuth"); ok {
		az.Set("supported", v)
	} else {
		az.Set("supported", "")
	}
	cl := must(c.DB.One("SELECT COUNT(*) FROM clubs")).IntAt(0)
	nn := must(c.Ugc.Count(""))
	return pyjson.Obj{{K: "motd", V: objAnyOr(cfg, "motd", "")}, {K: "news", V: objAnyOr(cfg, "news", []any{})}, {K: "heroImage", V: objAnyOr(cfg, "heroImage", "")},
		{K: "release", V: homeFeeds.get("release")}, {K: "azimuth", V: az}, {K: "rotation", V: objAny(ch, "rotation")}, {K: "events", V: objAny(ch, "events")},
		{K: "records", V: records}, {K: "cars", V: o.topCars(carDays, 5)},
		{K: "stats", V: pyjson.Obj{{K: "online", V: c.OnlineCount()}, {K: "accounts", V: must(c.Auth.Stats())["accounts"]}, {K: "clubs", V: cl}, {K: "runs", V: must(c.Sb.Count())}, {K: "designs", V: nn}}},
		{K: "time", V: time.Now().Unix()}}
}

func sortStable[T any](l []T, less func(a, b T) bool) {
	for i := 1; i < len(l); i++ {
		for j := i; j > 0 && less(l[j], l[j-1]); j-- {
			l[j], l[j-1] = l[j-1], l[j]
		}
	}
}
