package lsp

import (
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/notifstore"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/xls"
)

// Message Center timing.
const (
	pollSeconds     = 5
	pollIdleSeconds = 20 // 72 players at 5 s were 14 queries a second for a list that rarely changes
	pollBusyWindow  = 60 // back to pollSeconds for this long after any notification changes
	msgType         = 0x200
	notifMaxResults = 0x200
	notifMaxBlock   = 0xc800 // what the client asks for
	notifRepliesMax = 4096
)

// notifStructs maps a notificationType to its parameter struct, from the client's dispatcher: a call-struct name, or the descriptor address for the
// Car Club ones that have no registered name (build 1.0.0.64). Types not listed here are not dispatched by the client.
type nstruct struct {
	name string
	va   int64
}

var notifStructs = func() map[int64]nstruct {
	m := map[int64]nstruct{
		0x000: {"PermissionsDirtyNotificationParameters", 0}, 0x100: {"NewDlcNotificationParameters", 0},
		0x200: {"MessageNotificationParameters", 0}, 0x201: {"MessageNotificationParameters", 0}, 0x203: {"MessageNotificationParameters", 0},
		0x600: {"BannedNotificationParameters", 0}, 0x601: {"UnbannedNotificationParameters", 0}, 0x602: {"MessageNotificationParameters", 0},
		0x8000: {"UserBadgeGrantedNotificationParameters", 0}, 0x8001: {"SetCreditsNotificationParameters", 0},
		0x8302: {"CurationPayoutNotificationParameters", 0}, 0x8303: {"DrivatarPayoutNotificationParameters", 0}, 0x8401: {"MessageNotificationParameters", 0},
		0x8402: {"TimeTrialRecommendNotificationParameters", 0}, 0x8403: {"TimeTrialTimeBeatenNotificationParameters", 0},
		0x8500: {"GiftReceiveMoneyNotificationParameters", 0}, 0x8501: {"GiftReceiveItemNotificationParameters", 0},
		0x8502: {"GiftReceiveCarNotificationParameters", 0}, 0x8503: {"StorefrontOutOfStockNotificationParameters", 0},
		0x8504: {"StorefrontMoneyWaitingNotificationParameters", 0},
	}
	// Car Club notifications (Message Center category 7): 0x700 join request, 0x701 invite, 0x702 removal vote, 0x703 kicked, 0x704/0x705
	// {clubName, clubTag}, 0x706 member left, 0x707 new member, 0x708 promotion, 0x709 noticeboard message.
	for i := int64(0); i < 10; i++ {
		m[0x700+i] = nstruct{"", 0x1436ead20 + 0x10*i}
	}
	return m
}()

const structFallback = "MessageNotificationParameters"

// category is a Message Center tab, confirmed in game 2026-10-16.
type category struct {
	id         int64
	name, desc string
}

var categories = []category{{1, "New Content", "New downloadable content"}, {2, "Announcements", "Community news and announcements"},
	{3, "Rewards", "Payouts and rewards"}, {4, "Time Trials", "Time trial notices"}, {5, "Gifts", "Gifts from other players"},
	{6, "Messages", "Messages and account notices"}, {7, "Car Club", "Car club requests and notices"}}

// kindInfo is a notification kind of the admin API: its type and where it shows up.
type kindInfo struct {
	name  string
	ntype int64
	where string
}

var kinds = []kindInfo{
	{"announcement", 0x200, "Community News page"}, {"dlc", 0x100, "category 1"},
	{"reward_curation", 0x8302, "category 3 (shown as Layer Group Reward)"}, {"reward_drivatar", 0x8303, "category 3 (shown as Drivatar Reward)"},
	{"gift_money", 0x8500, "category 5"}, {"gift_item", 0x8501, "category 5 (GiftFileType 0 = tuning)"}, {"gift_car", 0x8502, "category 5"},
	{"message", 0x602, "category 6"}, {"ban", 0x600, "category 6 (applies a feature ban, undo with unban)"}, {"unban", 0x601, "category 6"},
	{"club_join_request", 0x700, "category 7"}, {"club_invite", 0x701, "category 7"}, {"club_removal_vote", 0x702, "category 7"},
	{"club_kicked", 0x703, "category 7"}, {"club_notice_a", 0x704, "category 7"}, {"club_notice_b", 0x705, "category 7"},
	{"club_member_left", 0x706, "category 7"}, {"club_new_member", 0x707, "category 7"}, {"club_promotion", 0x708, "category 7"},
	{"club_noticeboard", 0x709, "category 7"},
}

// notifFields is the field list of a notification type's parameter struct.
func (c *Core) notifFields(ntype int64) []*xls.Field {
	s, ok := notifStructs[ntype]
	if !ok {
		s = nstruct{structFallback, 0}
	}
	if s.name == "" {
		return c.SC.Fields(s.va)
	}
	return c.callStruct(s.name)
}

// AddNotification queues a Message Center entry.
func (c *Core) AddNotification(r *Req, message, sra string, ntype int64, params pyjson.Obj, xuid int64, carID *int64, giftID bool) (*notifstore.Notification, error) {
	n, err := c.Notif.Add(message, sra, ntype, params, xuid, carID, giftID)
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  + notification #%d type=%#x: %s", n.ID, n.Type, pyjson.PyRepr(message)))
	return n, nil
}

// ---------------------------------------------------------------- Logon

func (c *Core) allPermissions(n int) []any {
	out := make([]any, n)
	for i := 1; i <= n; i++ {
		s := xls.NewStruct()
		s.Set("methodId", int64(i))
		s.Set("tier", int64(0))
		s.Set("isEnabled", true)
		s.Set("access", int64(0))
		out[i-1] = s
	}
	return out
}

func init() { register("Forza.WebServices.User", "Logon", logon) }

func logon(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	lg := req.Sub("loginData")
	if r.Authed {
		lg.Set("xuid", uint64(r.Xuid)) // the account decides who this is, not what the client claims
		lg.Set("gamerTag", r.Tag)
	}
	xuid := lg.Int("xuid")
	c.SetTag(xuid, lg.Str("gamerTag"))
	level := cleanLevel(lg.Int("currentCareerLevel")) // preserve career XP for the client's level lookup
	lg.Set("currentCareerLevel", level)
	c.SetLevel(xuid, level)
	err := c.DB.Do(func(t *db.Tx) error { return db.TouchPlayer(t, xuid, lg.Str("gamerTag"), nil, &level) })
	if err != nil {
		return nil, err
	}
	shim := any(r.Shim)
	if r.Shim == "" {
		shim = "(none)"
	}
	var own any
	if r.OwnCars {
		own = true
	}
	c.Log(r, fmt.Sprintf("  Logon xuid=%#x tag=%s lcid=%d ", xuid, pyjson.PyRepr(lg.Str("gamerTag")), lg.Int("LCID"))+diag.KV(diag.KVP{K: "shim", V: shim},
		diag.KVP{K: "signed_in", V: r.Authed}, diag.KVP{K: "own_cars", V: own}, diag.KVP{K: "career_xp", V: level}))
	root := c.callStruct("ForzaUserLogon_Results")
	res := c.def(c.SC.Sub(root[0]))
	accepts, err := c.Clubs.AcceptsInvites(xuid)
	if err != nil {
		return nil, err
	}
	res.Set("isProfileValid", true)
	res.Set("notificationNonce", int64(1))
	res.Set("notificationPollIntervalSeconds", int64(5))
	res.Set("defaultFriendsPollIntervalSeconds", int64(60))
	res.Set("maxFriendsPollIntervalSeconds", int64(300))
	res.Set("defaultTimeoutMs", int64(30000))
	res.Set("defaultContentTimeoutMs", int64(40000))
	res.Set("defaultInGameTimeoutMs", int64(30000))
	ud := c.owner(res.Sub("userData"), xuid)
	ud.Set("wzGamerTag", lg.Str("gamerTag"))
	ud.Set("acceptsClubInvites", accepts)
	res.Set("userData", ud)
	// The client's permission table comes from here (it never calls GetPermissionsTable in practice); an empty table makes every feature gate read
	// "locked". Capacity of the array is 256 x 16 bytes.
	res.Set("permissions", c.allPermissions(255))
	// The client keeps at most 7 categories; it shows the ones with a tab (1, 3, 5, 6, 7) in the Message Center.
	var cats []any
	for _, ct := range categories {
		s := xls.NewStruct()
		s.Set("type", ct.id)
		s.Set("name", ct.name)
		s.Set("description", ct.desc)
		s.Set("backgroundImageFileId", int64(0))
		cats = append(cats, s)
	}
	res.Set("notificationCategories", cats)
	out := xls.NewStruct()
	out.Set("responseData", res)
	return out, nil
}

// ---------------------------------------------------------------- notifications (Message Center)

type cachedReply struct {
	generation int64
	reply      *xls.Struct
}

type notifState struct {
	mu         sync.Mutex
	replies    map[[3]int64]cachedReply
	lastGen    int64
	haveGen    bool
	lastChange time.Time
}

var notifCache = &notifState{replies: map[[3]int64]cachedReply{}}

func init() {
	register("Xls.WebServices.Notifications", "RetrieveForUser", notifRetrieve)
	register("Xls.WebServices.Notifications", "MarkRead", notifMarkRead)
	register("Xls.WebServices.Notifications", "Clear", notifClear)
	register("Xls.WebServices.Notifications", "StartNewSession", notifNewSession)
}

func notifRetrieve(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	limit, room := int64(notifMaxResults), int64(notifMaxBlock)
	if req != nil {
		if v := req.Int("maxResults"); v != 0 {
			limit = v
		}
		if v := req.Int("cbParameterBlockSize"); v != 0 {
			room = v
		}
	}
	key := [3]int64{r.Xuid, limit, room}
	gen := c.Notif.Generation()
	now := time.Now()
	st := notifCache
	st.mu.Lock()
	if !st.haveGen || st.lastGen != gen {
		st.lastGen, st.haveGen, st.lastChange = gen, true, now
	}
	poll := int64(pollSeconds)
	if now.Sub(st.lastChange).Seconds() >= pollBusyWindow {
		poll = pollIdleSeconds
	}
	if cached, ok := st.replies[key]; ok && cached.generation == gen {
		st.mu.Unlock()
		out := cached.reply.Clone()
		out.Set("nextRetrieveSeconds", poll)
		return out, nil
	}
	st.mu.Unlock()
	reply, err := c.notifReply(r, limit, room)
	if err != nil {
		return nil, err
	}
	st.mu.Lock()
	if len(st.replies) >= notifRepliesMax {
		st.replies = map[[3]int64]cachedReply{}
	}
	st.replies[key] = cachedReply{gen, reply}
	st.mu.Unlock()
	out := reply.Clone()
	out.Set("nextRetrieveSeconds", poll)
	return out, nil
}

// notifReply is the newest-first list up to the client's limits, in id order: broadcasts never expire and club notices are never cleared.
func (c *Core) notifReply(r *Req, limit, room int64) (*xls.Struct, error) {
	all, err := c.Notif.Visible(r.Xuid)
	if err != nil {
		return nil, err
	}
	var vis []*notifstore.Notification
	var results []any
	var blocks [][]byte
	for i := len(all) - 1; i >= 0; i-- {
		n := all[i]
		if int64(len(vis)) >= limit {
			break
		}
		fields := c.notifFields(n.Type)
		val := c.def(fields)
		if val.Has("Message") {
			val.Set("Message", n.Message)
		}
		if val.Has("SRAUri") {
			val.Set("SRAUri", n.Sra)
		}
		if val.Has("OfferId") {
			val.Set("OfferId", n.Sra)
		}
		for _, p := range n.Params {
			val.Set(p.K, p.V)
		}
		pb, err := c.SC.Encode(fields, val)
		if err != nil {
			return nil, err
		}
		if int64(len(pb)) > room {
			c.LogEvery(r, fmt.Sprintf("notif-room/%d", r.Xuid), 600, fmt.Sprintf("  notifications cut: %d delivered (newest first), the next one (type %#x, %d bytes) does not fit "+
				"the %d bytes left in the client's parameter block; it and the older ones are not delivered", len(vis), n.Type, len(pb), room), diag.Warn, "")
			break
		}
		room -= int64(len(pb))
		vis = append(vis, n)
		blocks = append(blocks, pb)
		res := xls.NewStruct()
		res.Set("id", n.ID)
		res.Set("xuidRecipient", n.Xuid)
		res.Set("notificationType", n.Type)
		res.Set("hasThumbnail", false)
		res.Set("isRead", n.Read)
		res.Set("deleteAction", int64(0))
		res.Set("expirationDate", notifstore.Filetime(c.Now()+86400*365))
		res.Set("sentDate", n.Sent)
		res.Set("cbParameters", int64(len(pb)))
		results = append(results, res)
	}
	// vis is newest first; results and ids go oldest first, the parameter block in the order the blocks were built reversed twice (as Python)
	for i, j := 0, len(vis)-1; i < j; i, j = i+1, j-1 {
		vis[i], vis[j] = vis[j], vis[i]
	}
	for i, j := 0, len(results)-1; i < j; i, j = i+1, j-1 {
		results[i], results[j] = results[j], results[i]
	}
	var block []byte
	for i := len(blocks) - 1; i >= 0; i-- {
		block = append(block, blocks[i]...)
	}
	ids := make([]any, len(vis))
	for i, n := range vis {
		ids[i] = n.ID
	}
	out := xls.NewStruct()
	if results == nil {
		results = []any{}
	}
	out.Set("results", results)
	out.Set("parameterBlock", block)
	out.Set("reconcileActiveNotifications", true)
	out.Set("activeNotificationIds", ids)
	return out, nil
}

func notifMarkRead(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	return nil, c.Notif.MarkRead(r.Xuid, req.Int("notificationId"))
}

// notifNewSession: the default result says "poll again in 0 s", a tight loop. Logon normally supplies these, this call is the client's fallback.
func notifNewSession(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	out := xls.NewStruct()
	out.Set("nonce", int64(1))
	out.Set("nextRetrieveSeconds", int64(pollSeconds))
	return out, nil
}

// notifClear clears it for this player only.
func notifClear(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	return nil, c.Notif.Clear(r.Xuid, req.Int("notificationId"))
}

// ---------------------------------------------------------------- notices that are never stored

const (
	outdatedNoticeID = 0x7FFFFF00 // only out-of-date clients see it
	banNoticeID      = 0x7FFFFF01
)

var fixedSent = notifstore.Filetime(0) // one fixed date, so every poll shows the same notice rather than a new one

// outdatedNotice is the Message Center reply for a client below minShim, in place of the HTTP 426 its game cannot show.
func (c *Core) outdatedNotice(need string) ([]byte, error) {
	return c.singleNotice(outdatedNoticeID, 0x200, "FH2MP is out of date, so online play is off. Close the game, open the FH2MP launcher and press Play: "+
		"it updates FH2MP to "+need+". Starting azimuth.exe directly skips the update.", nil)
}

// banNotice is the Message Center reply for a banned player's poll: the one message in the Messages tab with the end date and reason.
func (c *Core) banNotice(message string) ([]byte, error) {
	sent := fixedSent
	return c.singleNotice(banNoticeID, 0x602, message, &sent)
}

// singleNotice is a Notifications.RetrieveForUser reply holding one notice that is never stored, framed for the wire.
func (c *Core) singleNotice(nid, ntype int64, text string, sent *int64) ([]byte, error) {
	fields := c.notifFields(ntype)
	val := c.def(fields)
	val.Set("Message", text)
	pb, err := c.SC.Encode(fields, val)
	if err != nil {
		return nil, err
	}
	sd := fixedSent
	if sent != nil {
		sd = *sent
	}
	res := xls.NewStruct()
	res.Set("id", nid)
	res.Set("xuidRecipient", int64(0))
	res.Set("notificationType", ntype)
	res.Set("hasThumbnail", false)
	res.Set("isRead", false)
	res.Set("deleteAction", int64(0))
	res.Set("expirationDate", notifstore.Filetime(c.Now()+86400))
	res.Set("sentDate", sd)
	res.Set("cbParameters", int64(len(pb)))
	result := xls.NewStruct()
	result.Set("results", list(res))
	result.Set("parameterBlock", pb)
	result.Set("nextRetrieveSeconds", int64(60))
	result.Set("reconcileActiveNotifications", true)
	result.Set("activeNotificationIds", list(nid))
	_, rfields := c.findStruct("Xls.WebServices.Notifications, Xls.WebServices", "RetrieveForUser", "_Results")
	payload, err := c.SC.Encode(rfields, result)
	if err != nil {
		return nil, err
	}
	return frame(payload, 0, 0, nil), nil
}

// ---------------------------------------------------------------- admin parameter typing

func coerce(kind int64, v any) (any, error) {
	switch kind {
	case 9, 0x10:
		return pyjson.PyStr(v), nil
	case 0x11:
		if b, ok := v.(bool); ok {
			return b, nil
		}
		switch strings.ToLower(pyjson.PyStr(v)) {
		case "1", "true", "yes":
			return true, nil
		}
		return false, nil
	case 7:
		switch x := v.(type) {
		case float64:
			return x, nil
		case int64:
			return float64(x), nil
		}
		var f float64
		if _, err := fmt.Sscan(strings.TrimSpace(pyjson.PyStr(v)), &f); err != nil {
			return nil, fmt.Errorf("could not convert string to float: %s", pyjson.PyRepr(v))
		}
		return f, nil
	}
	switch x := v.(type) {
	case int64:
		return x, nil
	case bool:
		if x {
			return int64(1), nil
		}
		return int64(0), nil
	}
	n, err := parseIntBase0(pyjson.PyStr(v))
	if err != nil {
		return nil, fmt.Errorf("invalid literal for int() with base 0: %s", pyjson.PyRepr(pyjson.PyStr(v)))
	}
	return n, nil
}

// notifParams types the admin query extras (Field=value) for the type's parameter struct.
func (c *Core) notifParams(ntype int64, extra pyjson.Obj) (pyjson.Obj, error) {
	if len(extra) == 0 {
		return pyjson.Obj{}, nil
	}
	fields := c.notifFields(ntype)
	kinds := map[string]int64{}
	var names []string
	for _, f := range fields {
		kinds[f.Name] = f.Type
		names = append(names, f.Name)
	}
	var out pyjson.Obj
	for _, p := range extra {
		k, ok := kinds[p.K]
		if !ok {
			sort.Strings(names)
			sname := structFallback
			if s, has := notifStructs[ntype]; has {
				sname = s.name
				if sname == "" {
					sname = fmt.Sprintf("%d", s.va)
				}
			}
			q := make([]any, len(names))
			for i, n := range names {
				q[i] = n
			}
			return nil, fmt.Errorf("no field %s in %s; have %s", pyjson.PyRepr(p.K), sname, pyjson.PyRepr(q))
		}
		v, err := coerce(k, p.V)
		if err != nil {
			return nil, err
		}
		out = append(out, pyjson.Pair{K: p.K, V: v})
	}
	return out, nil
}
