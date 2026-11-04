// Package gamenet runs the game engine on real sockets: the UDP game port, the loopback control port, the optional TCP tunnel, and the HTTP calls
// to the LSP. Today it runs the Python loop (Hub.serve, fh2mp_server.py:5286) on one goroutine: control, drain datagrams, tick when due, flush, sweep.
package gamenet

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	neturl "net/url"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	mrand "math/rand/v2"

	"fh2mp/internal/catalog"
	"fh2mp/internal/diag"
	"fh2mp/internal/game"
	"fh2mp/internal/pyjson"
)

// Config is what main reads from argv and the environment.
type Config struct {
	Port     int
	LogPath  string
	Roadtrip bool
	Carmeet  bool
}

// Tunnel limits (tunnel.py).
const (
	maxFrame       = 2048
	recvBuffer     = 65536
	sendBacklog    = 48 * 1024
	maxConnections = 512
	perIP          = 3
	helloSeconds   = 10.0
	idleSeconds    = 60.0
)

type pcgRand struct{ r *mrand.Rand }

func newRand() *pcgRand {
	var seed [16]byte
	rand.Read(seed[:])
	return &pcgRand{mrand.New(mrand.NewPCG(binary.LittleEndian.Uint64(seed[:8]), binary.LittleEndian.Uint64(seed[8:])))}
}
func (p *pcgRand) Intn(n int) int   { return p.r.IntN(n) }
func (p *pcgRand) Float64() float64 { return p.r.Float64() }

// mux sends a datagram into a tunnel's TCP connection or over the UDP socket.
type mux struct {
	udp *net.UDPConn
	t   *tunnel
}

func toUDPAddr(a game.Addr) *net.UDPAddr {
	return &net.UDPAddr{IP: net.ParseIP(a.IP), Port: a.Port}
}

func (m *mux) SendTo(data []byte, addr game.Addr) error {
	if m.t != nil && m.t.has(addr) {
		m.t.send(data, addr)
		return nil
	}
	_, err := m.udp.WriteToUDP(data, toUDPAddr(addr))
	return err
}

type tconn struct {
	c       net.Conn
	addr    game.Addr
	born    float64
	lastRx  float64
	out     chan []byte
	pending atomic.Int64
	dead    bool
	why     string
	rx      int
	dropped int
}

type tunnel struct {
	mu    sync.RWMutex
	conns map[game.Addr]*tconn
}

func (t *tunnel) has(a game.Addr) bool {
	t.mu.RLock()
	defer t.mu.RUnlock()
	_, ok := t.conns[a]
	return ok
}

// HasConn implements game.TunnelHost.
func (t *tunnel) HasConn(a game.Addr) bool { return t.has(a) }

func (t *tunnel) send(data []byte, a game.Addr) bool {
	t.mu.RLock()
	c := t.conns[a]
	t.mu.RUnlock()
	if c == nil || c.dead {
		return false
	}
	if len(data) > maxFrame || c.pending.Load() > sendBacklog {
		c.dropped++
		if c.dropped == 1 || c.dropped%500 == 0 {
			diag.Get("tunnel").Warn(fmt.Sprintf("datagram dropped for port %d: %d bytes are above the frame limit or waiting to be sent (limit %d), the connection is too slow; %d dropped on this connection so far", a.Port, len(data), sendBacklog, c.dropped))
		}
		return true
	}
	frame := make([]byte, 2+len(data))
	binary.BigEndian.PutUint16(frame, uint16(len(data)))
	copy(frame[2:], data)
	c.pending.Add(int64(len(frame)))
	select {
	case c.out <- frame:
	default:
		c.pending.Add(-int64(len(frame)))
		c.dropped++
	}
	return true
}

type event struct {
	kind int // 0 datagram, 1 control, 2 tunnel accept, 3 tunnel frame, 4 tunnel closed, 5 stop
	data []byte
	addr game.Addr
	ctl  *net.UDPAddr
	conn net.Conn
	tc   *tconn
	why  string
}

// Server is a running game server.
type Server struct {
	cfg    Config
	hub    *game.Hub
	udp    *net.UDPConn
	ctl    *net.UDPConn
	tun    *tunnel
	tl     net.Listener
	clk    *game.RealClock
	events chan event
	logf   *os.File
}

func httpSinks() *game.Sinks {
	client := &http.Client{Timeout: 5 * time.Second}
	return &game.Sinks{
		Result: func(mode string, detail pyjson.Obj, log func(string)) {
			body, err := pyjson.Dumps(pyjson.Obj{{K: "mode", V: mode}, {K: "detail", V: detail}}, pyjson.Opts{})
			if err != nil {
				return
			}
			req, _ := http.NewRequest("POST", game.LspURL+"/admin/result", strings.NewReader(body))
			req.Header.Set("Content-Type", "application/json")
			resp, err := client.Do(req)
			if err != nil {
				log(fmt.Sprintf("event result not stored (%s): %v", mode, err))
				return
			}
			io.Copy(io.Discard, resp.Body)
			resp.Body.Close()
		},
		Notice: func(xuid uint64, text string, log func(string)) {
			q := neturl.Values{}
			q.Set("type", "0x602")
			q.Set("xuid", fmt.Sprintf("%#x", xuid))
			q.Set("text", text)
			resp, err := client.Get(game.LspURL + "/admin/send?" + q.Encode())
			if err != nil {
				log(fmt.Sprintf("notice for %#x not queued: %v", xuid, err))
				return
			}
			io.Copy(io.Discard, resp.Body)
			resp.Body.Close()
		},
		Gamemode: func(mode int, xuids []uint64) (string, string, error) {
			parts := make([]string, len(xuids))
			for i, x := range xuids {
				parts[i] = strconv.FormatUint(x, 10)
			}
			c := &http.Client{Timeout: 3 * time.Second}
			resp, err := c.Get(fmt.Sprintf("%s/admin/gamemode?type=%d&xuids=%s", game.LspURL, mode, strings.Join(parts, ",")))
			if err != nil {
				return "", "", err
			}
			defer resp.Body.Close()
			var doc struct{ Name, Template string }
			if err := json.NewDecoder(resp.Body).Decode(&doc); err != nil {
				return "", "", err
			}
			return doc.Name, doc.Template, nil
		},
	}
}

// New opens the sockets and builds the hub.
func New(cfg Config) (*Server, error) {
	s := &Server{cfg: cfg, clk: game.NewRealClock(), events: make(chan event, 8192)}
	addr := &net.UDPAddr{IP: net.IPv4zero, Port: cfg.Port}
	udp, err := net.ListenUDP("udp4", addr)
	if err != nil {
		return nil, err
	}
	udp.SetReadBuffer(4 << 20)
	s.udp = udp
	var ctl *net.UDPConn
	ctlPort := cfg.Port + 90
	if v := os.Getenv("FH2MP_CONTROL_PORT"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			ctlPort = n
		}
	}
	ctl, ctlErr := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: ctlPort})
	if ctlErr == nil {
		s.ctl = ctl
	}
	m := &mux{udp: udp}
	if tp, _ := strconv.Atoi(os.Getenv("FH2MP_TUNNEL_PORT")); tp != 0 {
		l, err := net.Listen("tcp4", fmt.Sprintf("0.0.0.0:%d", tp))
		if err != nil {
			return nil, err
		}
		s.tl = l
		s.tun = &tunnel{conns: map[game.Addr]*tconn{}}
		m.t = s.tun
	}
	var logw func(string) error
	if cfg.LogPath != "" {
		f, err := os.OpenFile(cfg.LogPath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		if err != nil {
			return nil, err
		}
		s.logf = f
		logw = func(line string) error { _, err := f.WriteString(line); return err }
	}
	cat, err := catalog.Load()
	if err != nil {
		return nil, err
	}
	hub, err := game.NewHub(game.HubConfig{Port: cfg.Port, LogPath: cfg.LogPath, Roadtrip: cfg.Roadtrip, Carmeet: cfg.Carmeet, Clock: s.clk, Rng: newRand(), Sock: m,
		Out: game.NewLogOut(logw), Sinks: httpSinks(), Catalog: cat, SyncPath: os.Getenv("FH2MP_SYNC_GAMEDB"),
		CtlSend: func(data []byte, to game.Addr) {
			if s.ctl != nil {
				if _, err := s.ctl.WriteToUDP(data, toUDPAddr(to)); err != nil {
					s.hub.Idle.LogEvery([2]any{"ctl-reply", "send"}, 60.0, fmt.Sprintf("control: reply not sent (%T: %v): the account service gets no answer", err, err), diag.Warn)
				}
			}
		}})
	if err != nil {
		return nil, err
	}
	s.hub = hub
	if s.tun != nil {
		hub.Tunnel = s.tun
	}
	ctlShown := "-"
	if s.ctl != nil {
		ctlShown = strconv.Itoa(s.ctl.LocalAddr().(*net.UDPAddr).Port)
	}
	extra := ""
	if s.tl != nil {
		extra = fmt.Sprintf(", tunnel tcp/%d", s.tl.Addr().(*net.TCPAddr).Port)
	}
	hub.Idle.Logf("hub listening on udp/%d, control udp/%s%s", cfg.Port, ctlShown, extra)
	if s.ctl == nil {
		hub.Idle.Error(fmt.Sprintf("control port udp/%d on 127.0.0.1 not bound (%v): the account service cannot send assign/auth/revoke/logout/pop to this server, so lobby placement, sign-in checks and kicks do not work", ctlPort, ctlErr))
	}
	return s, nil
}

func (s *Server) readUDP() {
	buf := make([]byte, 65535)
	for {
		n, from, err := s.udp.ReadFromUDP(buf)
		if err != nil {
			return
		}
		d := append([]byte(nil), buf[:n]...)
		s.events <- event{kind: 0, data: d, addr: game.Addr{IP: from.IP.String(), Port: from.Port}}
	}
}

func (s *Server) readCtl() {
	if s.ctl == nil {
		return
	}
	buf := make([]byte, 1024)
	for {
		n, from, err := s.ctl.ReadFromUDP(buf)
		if err != nil {
			return
		}
		s.events <- event{kind: 1, data: append([]byte(nil), buf[:n]...), ctl: from}
	}
}

func (s *Server) acceptTCP() {
	for {
		c, err := s.tl.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		s.events <- event{kind: 2, conn: c}
	}
}

func (s *Server) serveConn(tc *tconn) {
	go func() { // writer
		for f := range tc.out {
			tc.c.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if _, err := tc.c.Write(f); err != nil {
				s.events <- event{kind: 4, tc: tc, why: fmt.Sprintf("send error %v", err)}
				return
			}
			tc.pending.Add(-int64(len(f)))
		}
	}()
	buf := make([]byte, 0, recvBuffer)
	chunk := make([]byte, 65536)
	for {
		n, err := tc.c.Read(chunk)
		if n > 0 {
			buf = append(buf, chunk[:n]...)
			for len(buf) >= 2 {
				fl := int(binary.BigEndian.Uint16(buf))
				if fl > maxFrame {
					s.events <- event{kind: 4, tc: tc, why: fmt.Sprintf("frame of %d bytes announced, limit %d (not our framing, or a corrupt stream)", fl, maxFrame)}
					return
				}
				if len(buf) < 2+fl {
					break
				}
				if fl > 0 {
					s.events <- event{kind: 3, tc: tc, data: append([]byte(nil), buf[2:2+fl]...), addr: tc.addr}
				} else {
					s.events <- event{kind: 3, tc: tc, addr: tc.addr}
				}
				buf = buf[2+fl:]
			}
			if len(buf) > recvBuffer {
				s.events <- event{kind: 4, tc: tc, why: fmt.Sprintf("%d bytes buffered without a complete frame (limit %d)", len(buf), recvBuffer)}
				return
			}
		}
		if err != nil {
			why := "the client closed the connection"
			if err != io.EOF {
				why = fmt.Sprintf("receive error %v", err)
			}
			s.events <- event{kind: 4, tc: tc, why: why}
			return
		}
	}
}

func (s *Server) tunnelAccept(c net.Conn) {
	now := s.clk.Mono()
	ra := c.RemoteAddr().(*net.TCPAddr)
	ip := ra.IP.String()
	s.tun.mu.RLock()
	total, mine := len(s.tun.conns), 0
	for a := range s.tun.conns {
		if a.IP == ip {
			mine++
		}
	}
	s.tun.mu.RUnlock()
	if total >= maxConnections || mine >= perIP || !s.hub.TunnelAdmit(ip) {
		why := "no player is signed in from this address (UDP auth required)"
		if total >= maxConnections {
			why = fmt.Sprintf("the server holds the maximum of %d connections", maxConnections)
		} else if mine >= perIP {
			why = fmt.Sprintf("this address already has %d connections", perIP)
		}
		s.hub.Idle.LogEvery([2]any{"tunnel-refuse", why}, 60.0, "TCP connection refused: "+why, diag.Warn)
		c.Close()
		return
	}
	if tcp, ok := c.(*net.TCPConn); ok {
		tcp.SetNoDelay(true)
	}
	tc := &tconn{c: c, addr: game.Addr{IP: ip, Port: ra.Port}, born: now, lastRx: now, out: make(chan []byte, 4096)}
	s.tun.mu.Lock()
	s.tun.conns[tc.addr] = tc
	s.tun.mu.Unlock()
	go s.serveConn(tc)
}

func (s *Server) tunnelClose(tc *tconn, why string) {
	s.tun.mu.Lock()
	cur, ok := s.tun.conns[tc.addr]
	if ok && cur == tc {
		delete(s.tun.conns, tc.addr)
	}
	s.tun.mu.Unlock()
	if !ok || cur != tc {
		return
	}
	if tc.why == "" {
		tc.why = why
	}
	tc.dead = true
	diag.Get("tunnel").Info(fmt.Sprintf("TCP connection closed: port=%d %s %s", tc.addr.Port, tc.why,
		diag.KV(diag.KVP{K: "lived_s", V: int(s.clk.Mono() - tc.born)}, diag.KVP{K: "rx_bytes", V: tc.rx})))
	tc.c.Close()
	close(tc.out)
	s.hub.TunnelClosed(tc.addr)
}

func (s *Server) tunnelSweep(now float64) {
	if s.tun == nil {
		return
	}
	s.tun.mu.RLock()
	var all []*tconn
	for _, c := range s.tun.conns {
		all = append(all, c)
	}
	s.tun.mu.RUnlock()
	for _, c := range all {
		switch {
		case now-c.lastRx > idleSeconds:
			s.tunnelClose(c, fmt.Sprintf("silent for %.0f s (limit %g, the proxy sends a keep-alive every 15 s)", now-c.lastRx, idleSeconds))
		case now-c.born > helloSeconds && !s.hub.Opened(c.addr):
			s.tunnelClose(c, fmt.Sprintf("no game connection opened within %g s (the first datagrams were rejected, or none came)", helloSeconds))
		}
	}
}

// Run serves until a signal arrives.
func (s *Server) Run() error {
	go s.readUDP()
	go s.readCtl()
	if s.tl != nil {
		go s.acceptTCP()
	}
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	go func() { <-sig; s.events <- event{kind: 5} }()
	_ = context.Background
	lastSweep := 0.0
	nxtAt := 0.0
	timer := time.NewTimer(10 * time.Millisecond)
	defer timer.Stop()
	handle := func(ev event) bool {
		switch ev.kind {
		case 0:
			s.hub.Handle(ev.data, ev.addr)
		case 1:
			s.hub.ControlText(string(ev.data), &game.Addr{IP: ev.ctl.IP.String(), Port: ev.ctl.Port})
		case 2:
			s.tunnelAccept(ev.conn)
		case 3:
			ev.tc.lastRx = s.clk.Mono()
			ev.tc.rx += len(ev.data) + 2
			if len(ev.data) > 0 {
				s.hub.Handle(ev.data, ev.addr)
			}
		case 4:
			s.tunnelClose(ev.tc, ev.why)
		case 5:
			return false
		}
		return true
	}
	for {
		// control messages and datagrams first, then the timers (the Python loop's order)
		wait := 10 * time.Millisecond
		if d := time.Duration((nxtAt - s.clk.Mono()) * float64(time.Second)); d < wait {
			wait = d
		}
		if wait < 0 {
			wait = 0
		}
		select {
		case ev := <-s.events:
			if !handle(ev) {
				return s.shutdown()
			}
			for drained := false; !drained; {
				select {
				case ev := <-s.events:
					if !handle(ev) {
						return s.shutdown()
					}
				default:
					drained = true
				}
			}
		case <-time.After(wait):
		}
		nxtAt = s.hub.Step()
		now := s.clk.Mono()
		if now-lastSweep > 1.0 {
			lastSweep = now
			s.tunnelSweep(now)
		}
	}
}

func (s *Server) shutdown() error {
	s.hub.Log("shutting down")
	s.hub.Close()
	if s.logf != nil {
		s.logf.Sync()
	}
	return nil
}
