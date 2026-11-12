package mt

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"sync"
	"time"

	"github.com/gotd/td/mt"
)

// Message-id rules (core.telegram.org/mtproto/description#message-identifier-msg-id, /service_messages_about_messages): a client msg_id is
// about unixtime*2^32 and a multiple of 4; it may be at most 300 s old and 30 s ahead of the server clock. Each accepted id is remembered
// while it is inside that window, which is the replay protection: a repeat is answered from the result cache or dropped.
const (
	msgPast    = 300 * time.Second
	msgFuture  = 30 * time.Second
	maxSeen    = 4096
	maxResults = 64
	// A result larger than this is not cached for a repeated request.
	maxCachedResult = 64 << 10
)

// bad_msg_notification error codes.
const (
	badMsgIDTooLow      = 16
	badMsgIDTooHigh     = 17
	badMsgIDBits        = 18
	badMsgTooOld        = 20
	badSeqTooLow        = 32
	badSeqTooHigh       = 33
	badSeqEvenExpected  = 34
	badSeqOddExpected   = 35
	badServerSaltCode   = 48
	badContainerInvalid = 64
)

// msgIDGen makes server message ids: time based, strictly increasing, low bits 1 for a response and 3 for a message the client did not ask for.
type msgIDGen struct {
	mu   sync.Mutex
	last int64
	now  func() time.Time
}

func (g *msgIDGen) next(response bool) int64 {
	t := g.now()
	frac := uint64(t.Nanosecond()) << 32 / 1e9
	id := t.Unix()<<32 | int64(frac)
	id &^= 3
	low := int64(3)
	if response {
		low = 1
	}
	g.mu.Lock()
	if id <= g.last&^3 {
		id = g.last&^3 + 4
	}
	id |= low
	g.last = id
	g.mu.Unlock()
	return id
}

// msgTime is the unix time a msg_id names.
func msgTime(id int64) time.Time { return time.Unix(id>>32, 0) }

// Server salts change every hour and are derived, never stored: the salt of hour h for a key is the low 64 bits of
// HMAC-SHA256(server secret, auth_key_id || h). A salt is good from the start of its hour until 30 minutes after it ended; the first salt of
// the key exchange is good for 30 minutes after the key was made.
const saltHour = 3600

func saltFor(secret []byte, keyID [8]byte, hour int64) int64 {
	m := hmac.New(sha256.New, secret)
	m.Write(keyID[:])
	var b [8]byte
	binary.LittleEndian.PutUint64(b[:], uint64(hour))
	m.Write(b[:])
	return int64(binary.LittleEndian.Uint64(m.Sum(nil)))
}

func (s *Server) currentSalt(k *authKey, now time.Time) int64 {
	return saltFor(s.saltSecret[:], k.ak.ID, now.Unix()/saltHour)
}

func (s *Server) saltOK(k *authKey, salt int64, now time.Time) bool {
	h := now.Unix() / saltHour
	if salt == saltFor(s.saltSecret[:], k.ak.ID, h) {
		return true
	}
	if now.Unix()-h*saltHour < saltHour/2 && salt == saltFor(s.saltSecret[:], k.ak.ID, h-1) {
		return true
	}
	return salt == k.firstSalt && now.Sub(k.created) < 30*time.Minute
}

// futureSalts answers get_future_salts: n salts (1..64) from the current hour on.
func (s *Server) futureSalts(k *authKey, now time.Time, n int) []mt.FutureSalt {
	if n < 1 {
		n = 1
	}
	if n > 64 {
		n = 64
	}
	h := now.Unix() / saltHour
	out := make([]mt.FutureSalt, n)
	for i := range out {
		start := (h + int64(i)) * saltHour
		out[i] = mt.FutureSalt{ValidSince: int(start), ValidUntil: int(start + saltHour + saltHour/2), Salt: saltFor(s.saltSecret[:], k.ak.ID, h+int64(i))}
	}
	return out
}

// cachedResult is an rpc_result kept until the client acks it, so a repeated request gets the same answer instead of running again.
type cachedResult struct {
	body     []byte
	serverID int64
}

// session is one MTProto session (client-chosen session_id) of an auth key. All fields below mu are guarded by it; it is never held during
// network or database work.
type session struct {
	id      int64
	key     *authKey
	created time.Time

	mu         sync.Mutex
	conn       *clientConn
	layer      int
	appVersion string
	platform   string
	lastUsed   time.Time
	seen       map[int64]struct{}
	seenQ      []int64
	floor      int64
	maxID      int64
	maxSeq     int32
	outContent int32
	results    map[int64]*cachedResult
	resultQ    []int64
	bySrv      map[int64]int64
	pushes     map[int64]int
	acks       []int64
}

func newSession(id int64, k *authKey, now time.Time) *session {
	return &session{id: id, key: k, created: now, lastUsed: now, seen: map[int64]struct{}{}, results: map[int64]*cachedResult{},
		bySrv: map[int64]int64{}, pushes: map[int64]int{}}
}

// check applies the msg_id and seq_no rules to one client message; it returns a bad_msg_notification code (0 = accept) and whether the
// message is a repeat of one already accepted. Caller holds ss.mu.
func (ss *session) check(msgID int64, seq int32, content bool, now time.Time) (code int, dup bool) {
	if msgID&3 != 0 {
		return badMsgIDBits, false
	}
	t := msgTime(msgID)
	if now.Sub(t) > msgPast {
		return badMsgIDTooLow, false
	}
	if t.Sub(now) > msgFuture {
		return badMsgIDTooHigh, false
	}
	if _, ok := ss.seen[msgID]; ok {
		return 0, true
	}
	if msgID <= ss.floor {
		return badMsgTooOld, false
	}
	if content != (seq&1 == 1) {
		if content {
			return badSeqOddExpected, false
		}
		return badSeqEvenExpected, false
	}
	// A later message must not carry a lower seq_no, nor the same odd one (two content messages cannot share a number). Messages that arrive
	// out of order (a resend after a reconnect) skip this check: their place in the sequence is not known.
	if msgID > ss.maxID && ss.maxID != 0 {
		if seq < ss.maxSeq || (seq == ss.maxSeq && seq&1 == 1) {
			return badSeqTooLow, false
		}
	}
	return 0, false
}

// record remembers an accepted message; the oldest ids leave the set when it is full, raising the floor below which ids are refused.
func (ss *session) record(msgID int64, seq int32) {
	ss.seen[msgID] = struct{}{}
	ss.seenQ = append(ss.seenQ, msgID)
	if len(ss.seenQ) > maxSeen {
		old := ss.seenQ[0]
		ss.seenQ = ss.seenQ[1:]
		delete(ss.seen, old)
		if old > ss.floor {
			ss.floor = old
		}
	}
	if msgID > ss.maxID {
		ss.maxID, ss.maxSeq = msgID, seq
	}
}

// expireSeen drops ids that left the time window (they are refused by time from now on).
func (ss *session) expireSeen(now time.Time) {
	cut := now.Add(-msgPast).Unix() << 32
	i := 0
	for i < len(ss.seenQ) && ss.seenQ[i] < cut {
		delete(ss.seen, ss.seenQ[i])
		i++
	}
	ss.seenQ = ss.seenQ[i:]
}

func (ss *session) cacheResult(reqID, serverID int64, body []byte) {
	if len(body) > maxCachedResult {
		return
	}
	ss.results[reqID] = &cachedResult{body: body, serverID: serverID}
	ss.bySrv[serverID] = reqID
	ss.resultQ = append(ss.resultQ, reqID)
	for len(ss.resultQ) > maxResults {
		old := ss.resultQ[0]
		ss.resultQ = ss.resultQ[1:]
		if r := ss.results[old]; r != nil {
			delete(ss.bySrv, r.serverID)
			delete(ss.results, old)
		}
	}
}

// acked handles the client's msgs_ack: cached results it confirms are dropped; the highest pts of acked update pushes is returned (0 = none).
func (ss *session) acked(ids []int64) (pts int) {
	for _, id := range ids {
		if req, ok := ss.bySrv[id]; ok {
			delete(ss.bySrv, id)
			delete(ss.results, req)
		}
		if p, ok := ss.pushes[id]; ok {
			delete(ss.pushes, id)
			if p > pts {
				pts = p
			}
		}
	}
	return pts
}

func randInt64() int64 {
	var b [8]byte
	_, _ = rand.Read(b[:])
	return int64(binary.LittleEndian.Uint64(b[:]))
}
