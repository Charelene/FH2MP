package lsp

import (
	"fmt"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/auth"
	"fh2mp/internal/cardb"
	"fh2mp/internal/catalog"
	"fh2mp/internal/clubs"
	"fh2mp/internal/crashreports"
	"fh2mp/internal/db"
	"fh2mp/internal/drivatar"
	"fh2mp/internal/notifstore"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/rivals"
	"fh2mp/internal/scoreboard"
	"fh2mp/internal/social"
	"fh2mp/internal/ugc"
	"fh2mp/internal/xbl"
	"fh2mp/internal/xls"
)

// Setup connects to PostgreSQL and builds every store. dsn "" = FH2MP_PG_DSN.
func (c *Core) Setup(dsn string) error {
	d, err := db.Open(dsn)
	if err != nil {
		return err
	}
	c.DB = d
	has, err := d.One("SELECT 1 FROM information_schema.schemata WHERE schema_name='game'")
	if err != nil {
		return err
	}
	if has == nil {
		return fmt.Errorf("the game catalog is not loaded: build the bundle with tools/export_gamedata.py and load it with tools/load_gamedata_pg.py")
	}
	if c.SC, err = xls.LoadSchema(d); err != nil {
		return err
	}
	c.Cars = cardb.New(d)
	c.Log(nil, fmt.Sprintf("database %s: game catalog schema `game` present", d.Path))
	c.Notif = notifstore.New(d)
	c.Clubs = clubs.New(d)
	c.Social = social.New(d)
	c.Ugc = ugc.New(d)
	if cat, err := catalog.Load(); err == nil {
		c.Tables = cat.Parts // part ids of the game data (the catalog the game server checks lobby cars with)
	} else {
		return err
	}
	if err := c.SetupAuth(); err != nil {
		return err
	}
	fixed, err := c.Ugc.RepairLiveries()
	if err != nil {
		return err
	}
	if fixed > 0 {
		c.Log(nil, fmt.Sprintf("user content: repaired %d livery row(s) stored with the old upload prefix", fixed))
	}
	if fixed, err = c.Ugc.RepairPhotos(); err != nil {
		return err
	}
	if fixed > 0 {
		c.Log(nil, fmt.Sprintf("user content: converted %d photo(s) to 1920x1080", fixed))
	}
	b0, b1, err := c.Ugc.Recompress()
	if err != nil {
		return err
	}
	count, err := c.Ugc.Count("")
	if err != nil {
		return err
	}
	stored, err := c.Ugc.StoredBytes()
	if err != nil {
		return err
	}
	recomp := ""
	if b0 != 0 {
		recomp = fmt.Sprintf(" (recompressed %d -> %d)", b0, b1)
	}
	c.Log(nil, fmt.Sprintf("user content: %d files, %d bytes stored%s", count, stored, recomp))
	nn, err := c.Notif.Count()
	if err != nil {
		return err
	}
	c.Log(nil, fmt.Sprintf("%d notifications", nn))
	rows, err := d.Query("SELECT xuid, gamertag, career_level FROM players WHERE gamertag <> ''")
	if err != nil {
		return err
	}
	for i := range rows {
		c.SetTagDefault(rows[i].Int("xuid"), rows[i].Str("gamertag")) // identities survive a restart
		if lvl := rows[i].Int("career_level"); lvl != 0 {
			c.SetLevelDefault(rows[i].Int("xuid"), lvl)
		}
	}
	srows, err := d.Query("SELECT record FROM game.drivatar_shipped ORDER BY idx")
	if err != nil {
		return err
	}
	var shipped []drivatar.Record
	for i := range srows {
		v, err := pyjson.Parse(srows[i].Str("record"))
		if err != nil {
			return err
		}
		rec, _ := v.(pyjson.Obj)
		shipped = append(shipped, drivatar.Record(rec))
	}
	c.Drv = drivatar.New(shipped, d)
	c.Social.NameLookup = func(x int64) string { t, _ := c.Drv.TagForXuid(x); return t } // stored player Drivatars show their names, not Player#
	c.Xbl = xbl.New(xbl.NewUDPHub())
	c.Xbl.NameLookup = func(x int64) string { // session members carry their gamertag, not "Player 1" (GitHub #3)
		if t := c.clubTagOf(x); t != "" {
			return t
		}
		t, _ := c.Drv.TagForXuid(x)
		return t
	}
	dcount, err := c.Drv.Count()
	if err != nil {
		return err
	}
	c.Log(nil, fmt.Sprintf("drivatar store: %d shipped templates, %d stored players", len(shipped), dcount))
	var names []string
	for _, t := range shipped {
		if v, ok := t.Get("GamerTag"); ok {
			names = append(names, pyjson.PyStr(v))
		}
	}
	tags, err := c.Drv.Gamertags()
	if err != nil {
		return err
	}
	names = append(names, tags...)
	pool, err := c.Cars.FakePool()
	if err != nil {
		return err
	}
	c.Sb = scoreboard.New(d, names, pool)
	c.Sb.RouteLength = c.Cars.RouteLength
	c.Sb.EventInfo = func(id int64) *scoreboard.EventInfo {
		e := c.eventInfo(id)
		if e == nil {
			return nil
		}
		return &scoreboard.EventInfo{Track: e.Track, Laps: e.Laps, Cls: e.Cls, CarClassID: attrInt(e.Attrs, "CarClassId")}
	}
	c.Sb.EventCars = func(id int64) []cardb.PoolCar {
		e := c.eventInfo(id)
		if e == nil {
			return nil
		}
		cars, err := c.Riv.CarsFor(e.Attrs)
		if err != nil {
			panic(err)
		}
		return cars
	}
	c.Sb.EventCarIDs = func(id int64) map[int64]bool {
		e := c.eventInfo(id)
		if e == nil {
			return nil
		}
		var attrs rivals.Attrs
		for _, p := range e.Attrs {
			if p.K != "CarClassId" {
				attrs = append(attrs, p)
			}
		}
		cars, err := c.Riv.CarsFor(attrs)
		if err != nil {
			panic(err)
		}
		ids := map[int64]bool{}
		for _, car := range cars {
			ids[car.ID] = true
		}
		return ids
	}
	sbn, err := c.Sb.Count()
	if err != nil {
		return err
	}
	c.Log(nil, fmt.Sprintf("scoreboards: %d stored submissions", sbn))
	c.Social.BotName = func(x int64) string { // the rivals' ids resolve to their racing-driver names, not Player#
		if n := c.Sb.FakeName(x); n != "" {
			return n
		}
		return c.Drv.BotName(x)
	}
	c.Riv = rivals.New(c.Cars, d)
	n, err := c.Riv.EnforceStoredClasses()
	if err != nil {
		return err
	}
	c.Log(nil, fmt.Sprintf("rival events with the card class enforced: %d", n))
	if n, err = c.Riv.RefreshTexts(); err != nil {
		return err
	}
	c.Log(nil, fmt.Sprintf("rival event texts refreshed: %d", n))
	month, re, err := c.Riv.Serial(nil)
	if err != nil {
		return err
	}
	evs, err := c.Riv.Events()
	if err != nil {
		return err
	}
	c.Log(nil, fmt.Sprintf("rival events: %d this rotation (month %d, re-roll %d)", len(evs), month, re))
	c.Log(nil, fmt.Sprintf("schema loaded from the database: %d struct descriptors", c.SC.Count()))
	return nil
}

func attrInt(a rivals.Attrs, k string) *int64 {
	v, ok := a.Get(k)
	if !ok {
		return nil
	}
	n := toInt(v)
	return &n
}

// SetupAuth builds the identity service. FH2MP_ENV (prod|dev|test) names the realm that signatures are bound to and is checked against the database name,
// so a dev server can never be pointed at the production database by accident.
func (c *Core) SetupAuth() error {
	env := envOr("FH2MP_ENV", "prod")
	row, err := c.DB.One("SELECT current_database() AS name")
	if err != nil {
		return err
	}
	dbname := row.Str("name")
	if (env == "prod" && dbname != "fh2mp") || (env == "dev" && dbname == "fh2mp") {
		return fmt.Errorf("refusing to start: FH2MP_ENV=%s but the database is %s", env, pyjson.PyRepr(dbname))
	}
	// required unless the environment says otherwise: a missing variable must not open the server (audit 2026-10-25 F08)
	svc, err := auth.NewService(c.DB, env, envOr("FH2MP_AUTH", "required"), envOr("FH2MP_REGISTRATION", "open"), envOr("FH2MP_TOKEN_IP", "exact"))
	if err != nil {
		return fmt.Errorf("fh2mp LSP: %v", err)
	}
	c.Auth = svc
	svc.OnChange = func(xuid int64) {
		if r, err := c.DB.One("SELECT gamertag FROM accounts WHERE xuid=?", xuid); err == nil && r != nil {
			c.SetTag(xuid, r.Str("gamertag"))
		}
	}
	svc.OnRevoke = c.OnRevoke
	rows, err := c.DB.Query("SELECT xuid, gamertag FROM accounts")
	if err != nil {
		return err
	}
	for i := range rows {
		c.SetTag(rows[i].Int("xuid"), rows[i].Str("gamertag"))
	}
	c.setupLauncherAPI()
	if c.setupLauncher != nil {
		if err := c.setupLauncher(c); err != nil {
			return err
		}
	}
	st, err := svc.Stats()
	if err != nil {
		return err
	}
	c.Log(nil, fmt.Sprintf("auth: env=%s mode=%s registration=%s accounts=%d", env, svc.Mode, svc.Registration, st["accounts"]))
	return nil
}

func envOr(k, d string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return d
}

// OnlineCount is the number of players whose client polled lately.
func (c *Core) OnlineCount() int { return social.OnlineCount() }

// limitListener allows at most n connections at once (audit 2026-10-25 F11): when they are all busy, Accept waits for one to end instead of serving
// without limit.
type limitListener struct {
	net.Listener
	slots chan struct{}
}

type limitConn struct {
	net.Conn
	once sync.Once
	free func()
}

func (c *limitConn) Close() error {
	err := c.Conn.Close()
	c.once.Do(c.free)
	return err
}

func (l *limitListener) Accept() (net.Conn, error) {
	l.slots <- struct{}{}
	conn, err := l.Listener.Accept()
	if err != nil {
		<-l.slots
		return nil, err
	}
	return &limitConn{Conn: conn, free: func() { <-l.slots }}, nil
}

// Serve runs the LSP on bind:port until the listener fails.
func (c *Core) Serve(bind string, port int) error {
	threads := 256
	if s := os.Getenv("FH2MP_LSP_THREADS"); s != "" {
		if n, err := parseIntBase0(s); err == nil && n > 0 {
			threads = int(n)
		}
	}
	timeout := 30 * time.Second // a client that sends nothing for this long (slow body, idle keep-alive) loses its connection
	if s := os.Getenv("FH2MP_LSP_READ_TIMEOUT"); s != "" {
		var f float64
		if _, err := fmt.Sscan(s, &f); err == nil && f > 0 {
			timeout = time.Duration(f * float64(time.Second))
		}
	}
	ln, err := net.Listen("tcp", net.JoinHostPort(bind, fmt.Sprint(port)))
	if err != nil {
		return err
	}
	if bind != "127.0.0.1" && bind != "::1" && bind != "localhost" {
		c.Log(nil, fmt.Sprintf("WARNING: listening on %s, not loopback: put the proxy in front (ops/nginx) and keep this port closed to the internet", bind))
	}
	srv := &http.Server{Handler: c, ReadTimeout: timeout, IdleTimeout: timeout, ErrorLog: nil}
	c.Log(nil, fmt.Sprintf("fh2mp LSP listening on %s:%d", bind, port))
	return srv.Serve(&limitListener{Listener: ln, slots: make(chan struct{}, threads)})
}

var _ = strings.Join

// setupLauncherAPI builds the /v1 API around the identity service.
func (c *Core) setupLauncherAPI() {
	host := publicHost()
	l := NewLauncher(c)
	url := os.Getenv("FH2MP_LSP_URL")
	if url == "" {
		port := os.Getenv("FH2MP_LSP_PUBLIC_PORT")
		if port == "" {
			port = "5080"
		}
		url = fmt.Sprintf("http://%s:%s", host, port)
	}
	l.LspPub = pyjson.Obj{{K: "url", V: url}}
	port := int64(5000)
	if s := os.Getenv("FH2MP_GAME_PORT"); s != "" {
		port = p10(s)
	}
	l.GamePub = pyjson.Obj{{K: "host", V: host}, {K: "port", V: port}}
	if s := os.Getenv("FH2MP_GAME_TCP_PORT"); s != "" {
		l.GamePub.Set("tcp", p10(s)) // the game server's experimental TCP carrier (FH2MP_TUNNEL_PORT)
	}
	if os.Getenv("FH2MP_GAME_PROBE") == "1" {
		l.GamePub.Set("probe", int64(1)) // the game server answers UDP reachability probes: launchers may then switch to TCP by themselves
	}
	if os.Getenv("FH2MP_NET_TRAFFIC") == "1" {
		l.GamePub.Set("traffic", int64(1)) // launchers start the game with FH2MP_NET_TRAFFIC=1: the shim keeps the game's networked traffic
	}
	l.Crashes = crashreports.FromEnv(func(m string) { c.Log(nil, m) })
	l.ClubInv = c.LauncherClubInvite
	c.Launcher = l
}
