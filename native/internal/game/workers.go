package game

import (
	"encoding/binary"
	"math"
	"sync"
	"time"
)

// Multi-core model (plan Part C): every world is owned by exactly one worker goroutine, which handles that world's datagrams, ticks it and
// flushes its queues. The hub goroutine receives datagrams and routes them; anything that touches several worlds or the hub's tables
// (a join, a ticket, a control command, a kick, the 1 s sweep) runs in Hub.stop: every worker first finishes what was queued before it and then
// waits, so the operation sees the whole server at rest. Hub tables are therefore changed only while the workers wait, and only read while they run.
// With no workers (FH2MP_WORKERS=1) none of this exists and Hub.Handle runs everything on the calling goroutine, as Python does.

type jobKind int

const (
	jobData jobKind = iota
	jobLight
	jobHonk
	jobPause
	jobStep
	jobTick
	jobFlush
	jobQuit
)

type job struct {
	kind   jobKind
	w      *World
	data   []byte
	addr   Addr
	ack    *sync.WaitGroup
	resume chan struct{}
	now    float64
}

// Worker runs a set of worlds on one goroutine.
type Worker struct {
	h      *Hub
	inbox  chan job
	worlds []*World
	done   chan struct{}
}

// StartWorkers switches the hub to n worker goroutines (n >= 2 is useful; 1 would only add a hand-off). manual = no timer: the caller drives
// ticks and flushes with StepWorkers (tests).
func (h *Hub) StartWorkers(n int, manual bool) {
	h.manual = manual
	for i := 0; i < n; i++ {
		wk := &Worker{h: h, inbox: make(chan job, 16384), done: make(chan struct{})}
		h.workers = append(h.workers, wk)
		go wk.run()
	}
	for _, w := range h.Worlds.Values() {
		h.attachWorld(w)
	}
}

// StopWorkers ends the worker goroutines.
func (h *Hub) StopWorkers() {
	for _, wk := range h.workers {
		wk.inbox <- job{kind: jobQuit}
		<-wk.done
	}
	h.workers = nil
}

// attachWorld gives a new world to the least loaded worker.
func (h *Hub) attachWorld(w *World) {
	if len(h.workers) == 0 || w.worker != nil {
		return
	}
	best := h.workers[0]
	for _, wk := range h.workers[1:] {
		if len(wk.worlds) < len(best.worlds) {
			best = wk
		}
	}
	best.worlds = append(best.worlds, w)
	w.worker = best
}

func (h *Hub) detachWorld(w *World) {
	wk := w.worker
	if wk == nil {
		return
	}
	for i, x := range wk.worlds {
		if x == w {
			wk.worlds = append(wk.worlds[:i:i], wk.worlds[i+1:]...)
			break
		}
	}
	w.worker = nil
}

func (wk *Worker) run() {
	defer close(wk.done)
	var tick <-chan time.Time
	if !wk.h.manual {
		t := time.NewTicker(5 * time.Millisecond)
		defer t.Stop()
		tick = t.C
	}
	for {
		select {
		case j := <-wk.inbox:
			if !wk.do(j) {
				return
			}
		case <-tick:
			wk.pass(wk.h.Clk.Mono())
		}
	}
}

// pass is one turn of the Python serve loop for this worker's worlds: tick when due, then flush.
func (wk *Worker) pass(now float64) {
	now = wk.tickPhase(now)
	wk.flushPhase(now)
}

func (wk *Worker) tickPhase(now float64) float64 {
	for _, w := range wk.worlds {
		if now >= w.nextTick {
			w.nextTick = now + WorldTickSeconds
			wk.h.tickSafe(w)
			now = wk.h.Clk.Mono()
		}
	}
	return now
}

func (wk *Worker) flushPhase(now float64) {
	for _, w := range wk.worlds {
		w.Flush(now)
	}
}

func (wk *Worker) do(j job) bool {
	switch j.kind {
	case jobData:
		wk.h.handleSafe(j.w, j.data, j.addr)
	case jobLight:
		wk.h.lightingHandle(j.data, j.addr, j.w)
	case jobHonk:
		wk.h.honkHandle(j.data, j.addr, j.w)
	case jobPause:
		j.ack.Done()
		<-j.resume
	case jobStep:
		wk.pass(j.now)
		j.ack.Done()
	case jobTick:
		wk.tickPhase(j.now)
		j.ack.Done()
	case jobFlush:
		wk.flushPhase(j.now)
		j.ack.Done()
	case jobQuit:
		return false
	}
	return true
}

// stop runs fn with every worker idle (after finishing what was queued before).
func (h *Hub) stop(fn func()) {
	if len(h.workers) == 0 {
		fn()
		return
	}
	var wg sync.WaitGroup
	resume := make(chan struct{})
	wg.Add(len(h.workers))
	for _, wk := range h.workers {
		wk.inbox <- job{kind: jobPause, ack: &wg, resume: resume}
	}
	wg.Wait()
	defer close(resume)
	fn()
}

// StepWorkers runs one pass on every worker at the given time and waits for it (manual mode): all worlds tick, then what the ticks asked of the
// hub (kicks) runs, then all worlds flush, which is the order of Python's loop.
func (h *Hub) StepWorkers(now float64) {
	var wg sync.WaitGroup
	wg.Add(len(h.workers))
	for _, wk := range h.workers {
		wk.inbox <- job{kind: jobTick, now: now, ack: &wg}
	}
	wg.Wait()
	h.RunDeferred()
	wg.Add(len(h.workers))
	for _, wk := range h.workers {
		wk.inbox <- job{kind: jobFlush, now: now, ack: &wg}
	}
	wg.Wait()
}

// Defer runs fn in the hub's quiet state: at once when there are no workers, otherwise at the next RunDeferred.
func (h *Hub) Defer(fn func()) {
	if len(h.workers) == 0 {
		fn()
		return
	}
	h.deferMu.Lock()
	h.deferred = append(h.deferred, fn)
	h.deferMu.Unlock()
}

// RunDeferred runs what the worlds asked the hub to do (kicks, assignments), with the workers idle.
func (h *Hub) RunDeferred() {
	h.deferMu.Lock()
	fns := h.deferred
	h.deferred = nil
	h.deferMu.Unlock()
	if len(fns) == 0 {
		return
	}
	h.stop(func() {
		for _, fn := range fns {
			fn()
		}
	})
}

// HandleDatagram is Hub.Handle for a hub with workers: established connections go straight to their world's worker; everything that
// changes hub state waits for the workers to be idle first.
func (h *Hub) HandleDatagram(data []byte, addr Addr) {
	if len(h.workers) == 0 {
		h.Handle(data, addr)
		return
	}
	switch {
	case len(data) >= 19 && string(data[11:19]) == lightingMagic:
		h.sideDispatch(data, addr, jobLight)
		return
	case len(data) >= 19 && string(data[11:19]) == honkMagic:
		h.sideDispatch(data, addr, jobHonk)
		return
	case len(data) == probeLength && string(data[11:19]) == probeMagic:
		h.probeHandle(data, addr)
		return
	case len(data) == TicketLen && string(data[11:19]) == TicketMagic:
		h.stop(func() { h.Handle(data, addr) })
		return
	}
	if _, _, opened := h.OpenConnectionFields(data); opened {
		h.stop(func() { h.Handle(data, addr) })
		return
	}
	w, ok := h.Route.Get(addr)
	if !ok || w == nil || w.worker == nil {
		h.handleSafe(h.Idle, data, addr) // strangers: the idle world keeps no peer and runs here
		return
	}
	w.worker.inbox <- job{kind: jobData, w: w, data: data, addr: addr}
}

// sideDispatch hands a lighting / horn request to the one world its player's ticket is bound in; anything unclear runs with the workers idle.
func (h *Hub) sideDispatch(data []byte, addr Addr, kind jobKind) {
	if len(data) < 28 {
		return
	}
	xuid := binary.LittleEndian.Uint64(data[20:])
	var world *World
	multiple := false
	for a, b := range h.Bound {
		if b.xuid != xuid {
			continue
		}
		if cw, ok := h.Route.Get(a); ok && cw != nil {
			if world != nil && world != cw {
				multiple = true
			}
			world = cw
		}
	}
	if world == nil || multiple || world.worker == nil {
		h.stop(func() {
			if kind == jobLight {
				h.lightingHandle(data, addr, nil)
			} else {
				h.honkHandle(data, addr, nil)
			}
		})
		return
	}
	world.worker.inbox <- job{kind: kind, w: world, data: data, addr: addr}
}

// StepHub runs the hub's own timers in a hub with workers: the idle world's flush, the deferred requests and the 1 s sweep. It returns when the next
// idle flush is due.
func (h *Hub) StepHub() float64 {
	now := h.Clk.Mono()
	h.RunDeferred()
	nxt := h.Idle.Flush(now)
	if now-h.lastSweep > 1.0 {
		h.lastSweep = now
		h.stop(h.Sweep)
	}
	if math.IsInf(nxt, 1) {
		return nxt
	}
	return nxt
}
