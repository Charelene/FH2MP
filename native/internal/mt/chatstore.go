package mt

import (
	"bytes"
	"sort"
	"sync"
	"time"
)

// ChatState is where a secret chat is.
type ChatState int

const (
	ChatRequested ChatState = 0
	ChatAccepted  ChatState = 1
	ChatDiscarded ChatState = 2
)

func (s ChatState) String() string {
	switch s {
	case ChatRequested:
		return "requested"
	case ChatAccepted:
		return "accepted"
	}
	return "discarded"
}

// SentTTL is how long a relayed message's franking commitment is kept for a report; a discarded chat's row goes after it too.
const SentTTL = 30 * 24 * time.Hour

// ChatDevice is an account's current chat device: its Ed25519 key and the account key's certificate over it.
type ChatDevice struct {
	User, Device       int64
	Pubkey, AccountKey []byte
	Signature          []byte
	Date               int
}

// SecretChat is a secret chat as the server keeps it: the two ends with their devices and the public halves of the key exchange with
// their device signatures. Never a key.
type SecretChat struct {
	ID                                                 int32
	Date                                               int
	Admin, AdminDevice, Participant, ParticipantDevice int64
	RandomID                                           int32
	State                                              ChatState
	GA, GASig, GB, GBSig                               []byte
	Fingerprint                                        int64
	changed                                            time.Time
}

// Has says whether x is one of the chat's two ends.
func (c *SecretChat) Has(x int64) bool { return x == c.Admin || x == c.Participant }

// Other is the end that is not x.
func (c *SecretChat) Other(x int64) int64 {
	if x == c.Admin {
		return c.Participant
	}
	return c.Admin
}

// SentRecord is what is kept of a relayed sealed message for 30 days: who sent it to whom, its random_id and the franking commitment.
type SentRecord struct {
	Chat              int32
	Sender, Recipient int64
	RandomID          int64
	Commitment        []byte
	Date              int
}

// Report is one row of the operators' report list: a message whose recipient showed its text and franking key, and they matched.
type Report struct {
	ID               int64
	Created          time.Time
	Reporter, Sender int64
	Chat             int32
	RandomID         int64
	Text, Reason     string
}

// ChatStore keeps the server's side of end-to-end direct messages (devices, secret chats, commitments, reports). The sealed bytes live in
// the UpdateStore as the recipient's updates.
type ChatStore interface {
	// Device is the user's current chat device (nil: none registered).
	Device(user int64) (*ChatDevice, error)
	// SetDevice makes d the user's current chat device and returns the one it replaced (nil: none).
	SetDevice(d ChatDevice) (old *ChatDevice, err error)
	// NewChat stores c as a requested chat with a fresh positive id. A chat of the same admin, random_id and participant with the same g_a
	// is returned instead, and made is false (a repeated request after a lost answer).
	NewChat(c SecretChat, now time.Time) (chat SecretChat, made bool, err error)
	// Chat is the chat with id (nil: none).
	Chat(id int32) (*SecretChat, error)
	// Accept moves a requested chat to accepted; false when it was not requested.
	Accept(id int32, gB, gBSig []byte, fingerprint int64, now time.Time) (bool, error)
	// Discard moves a chat to discarded; false when it already was or does not exist.
	Discard(id int32, now time.Time) (bool, error)
	// OpenChats lists the user's chats that are not discarded, on either end; other != 0 keeps only those with other.
	OpenChats(user, other int64) ([]SecretChat, error)
	// AddSent keeps a relayed message's record. When the sender already sent random_id on the chat nothing is stored: the first record's
	// date comes back with fresh = false.
	AddSent(m SentRecord, now time.Time) (date int, fresh bool, err error)
	// DeleteSent takes a record back (its message could not be stored).
	DeleteSent(chat int32, sender, randomID int64) error
	// SentTo is the record of a message random_id on chat that recipient received (nil: none kept).
	SentTo(chat int32, randomID, recipient int64) (*SentRecord, error)
	// AddReport puts a matched report on the list; false when this reporter reported this message already.
	AddReport(r Report) (bool, error)
	// Reports lists the newest reports first.
	Reports(limit int) ([]Report, error)
	// Sweep drops commitments older than SentTTL and chats discarded longer ago than that.
	Sweep(now time.Time) (sent, chats int64, err error)
}

// MemChatStore is a ChatStore in memory (tests, the test server).
type MemChatStore struct {
	mu      sync.Mutex
	devices map[int64]ChatDevice
	chats   map[int32]*SecretChat
	next    int32
	sent    map[[3]int64]memSent
	reports []Report
}

type memSent struct {
	r       SentRecord
	created time.Time
}

// NewMemChatStore makes an empty store.
func NewMemChatStore() *MemChatStore {
	return &MemChatStore{devices: map[int64]ChatDevice{}, chats: map[int32]*SecretChat{}, sent: map[[3]int64]memSent{}}
}

func cloneBytes(b []byte) []byte {
	if b == nil {
		return nil
	}
	return append([]byte(nil), b...)
}

func (m *MemChatStore) Device(user int64) (*ChatDevice, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	d, ok := m.devices[user]
	if !ok {
		return nil, nil
	}
	return &d, nil
}

func (m *MemChatStore) SetDevice(d ChatDevice) (*ChatDevice, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	old, had := m.devices[d.User]
	d.Pubkey, d.AccountKey, d.Signature = cloneBytes(d.Pubkey), cloneBytes(d.AccountKey), cloneBytes(d.Signature)
	m.devices[d.User] = d
	if !had {
		return nil, nil
	}
	return &old, nil
}

func (m *MemChatStore) NewChat(c SecretChat, now time.Time) (SecretChat, bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, o := range m.chats {
		if o.Admin == c.Admin && o.RandomID == c.RandomID && o.Participant == c.Participant && bytes.Equal(o.GA, c.GA) {
			return *o, false, nil
		}
	}
	m.next++
	c.ID, c.State, c.changed = m.next, ChatRequested, now
	c.GA, c.GASig, c.GB, c.GBSig = cloneBytes(c.GA), cloneBytes(c.GASig), nil, nil
	m.chats[c.ID] = &c
	return c, true, nil
}

func (m *MemChatStore) Chat(id int32) (*SecretChat, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	c, ok := m.chats[id]
	if !ok {
		return nil, nil
	}
	cp := *c
	return &cp, nil
}

func (m *MemChatStore) Accept(id int32, gB, gBSig []byte, fp int64, now time.Time) (bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	c, ok := m.chats[id]
	if !ok || c.State != ChatRequested {
		return false, nil
	}
	c.State, c.GB, c.GBSig, c.Fingerprint, c.changed = ChatAccepted, cloneBytes(gB), cloneBytes(gBSig), fp, now
	return true, nil
}

func (m *MemChatStore) Discard(id int32, now time.Time) (bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	c, ok := m.chats[id]
	if !ok || c.State == ChatDiscarded {
		return false, nil
	}
	c.State, c.changed = ChatDiscarded, now
	return true, nil
}

func (m *MemChatStore) OpenChats(user, other int64) ([]SecretChat, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var out []SecretChat
	for _, c := range m.chats {
		if c.State != ChatDiscarded && c.Has(user) && (other == 0 || c.Has(other)) {
			out = append(out, *c)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out, nil
}

func (m *MemChatStore) AddSent(r SentRecord, now time.Time) (int, bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	k := [3]int64{int64(r.Chat), r.Sender, r.RandomID}
	if o, ok := m.sent[k]; ok {
		return o.r.Date, false, nil
	}
	r.Commitment = cloneBytes(r.Commitment)
	m.sent[k] = memSent{r: r, created: now}
	return r.Date, true, nil
}

func (m *MemChatStore) DeleteSent(chat int32, sender, randomID int64) error {
	m.mu.Lock()
	delete(m.sent, [3]int64{int64(chat), sender, randomID})
	m.mu.Unlock()
	return nil
}

func (m *MemChatStore) SentTo(chat int32, randomID, recipient int64) (*SentRecord, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, s := range m.sent {
		if s.r.Chat == chat && s.r.RandomID == randomID && s.r.Recipient == recipient {
			r := s.r
			return &r, nil
		}
	}
	return nil, nil
}

func (m *MemChatStore) AddReport(r Report) (bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, o := range m.reports {
		if o.Reporter == r.Reporter && o.Chat == r.Chat && o.RandomID == r.RandomID {
			return false, nil
		}
	}
	r.ID = int64(len(m.reports) + 1)
	m.reports = append(m.reports, r)
	return true, nil
}

func (m *MemChatStore) Reports(limit int) ([]Report, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var out []Report
	for i := len(m.reports) - 1; i >= 0 && len(out) < limit; i-- {
		out = append(out, m.reports[i])
	}
	return out, nil
}

func (m *MemChatStore) Sweep(now time.Time) (int64, int64, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	cut := now.Add(-SentTTL)
	var sent, chats int64
	for k, s := range m.sent {
		if s.created.Before(cut) {
			delete(m.sent, k)
			sent++
		}
	}
	for id, c := range m.chats {
		if c.State == ChatDiscarded && c.changed.Before(cut) {
			delete(m.chats, id)
			chats++
		}
	}
	return sent, chats, nil
}
