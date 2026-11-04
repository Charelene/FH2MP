package lsp

import (
	"fmt"
	"math"
	"strings"

	"fh2mp/internal/clubs"
	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/rivals"
	"fh2mp/internal/scoreboard"
	"fh2mp/internal/xls"
)

const (
	monthlyCategory = 4 // the Monthly Rivals page lists GetTimeTrials entries of category 4; other categories are unmapped
	topClass        = 5 // classes 6 (X) and 7 (R) exist in the game DB but the client reports them for maxed cars; relayed as S2 [display choice]
)

func init() {
	const sbc = "Forza.WebServices.Scoreboard"
	register(sbc, "SubmitScore", sbSubmitScore)
	register(sbc, "UploadFile", sbUploadFile)
	register(sbc, "NotifyRival", sbNotifyRival)
	register(sbc, "DownloadFile", sbDownloadFile)
	register(sbc, "SubmitXpScore", sbSubmitXP)
	register(sbc, "GetBestScore", sbBest)
	register(sbc, "GetRival", sbRival)
	register(sbc, "GetRowsAnthem", func(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
		return sbRows(c, r, req, "ForzaWebServicesScoreboardGetRowsAnthem_Results")
	})
	register(sbc, "GetRows", func(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
		return sbRows(c, r, req, "ForzaWebServicesScoreboardGetRows_Results")
	})
	register(sbc, "GetTimeTrials", sbTimeTrials)
	register("Forza.WebServices.Hopper", "GetGameOptions", hopperGameOptions)
}

// scoreRow fills a ScoreboardRow from a board row; infos is CLUBS.member_infos of the page (nil = look the player up alone).
func (c *Core) scoreRow(row *scoreboard.Row, rd *xls.Struct, infos map[int64]clubs.Info) *xls.Struct {
	user := rd.Sub("user")
	lvl := cleanLevel(c.Level(row.Xuid))
	if infos == nil && c.Clubs != nil {
		infos, _ = c.Clubs.MemberInfos([]int64{row.Xuid})
	}
	info := infos[row.Xuid]
	tag := row.Tag
	if tag == "" {
		tag = c.Tag(row.Xuid)
	}
	user.Set("qwXuid", row.Xuid)
	user.Set("wzGamerTag", cut(tag, 15))
	user.Set("currentCareerLevel", lvl)
	user.Set("clubId", info.ClubID)
	user.Set("clubTag", cut(info.Tag, 9))
	user.Set("roleInClub", clubs.WireRole(info.Role))
	rd.Set("position", int64(row.Position))
	rd.Set("rank", int64(row.Position))
	rd.Set("score", scoreboard.DblToU64(row.Score))
	rd.Set("submittedTime", scoreboard.Iso(row.T))
	rd.Set("carId", row.Car)
	rd.Set("carClassId", min(row.Cls, topClass))
	rd.Set("carPerformanceIndex", row.Cpi)
	rd.Set("usedSTM", row.Stm)
	rd.Set("usedTCS", row.Tcs)
	rd.Set("usedABS", row.Abs)
	rd.Set("usedAutoBrake", row.Autobrake)
	rd.Set("usedAutoShifting", row.Autoshift)
	rd.Set("usedClutch", row.Clutch)
	rd.Set("isClean", row.Clean)
	rd.Set("ghostFileId", row.Ghost)
	rd.Set("bounty", int64(0))
	return rd
}

func (c *Core) eventInfo(id int64) *rivals.Event {
	if c.Riv == nil {
		return nil
	}
	e, err := c.Riv.ByID(id)
	if err != nil {
		return nil
	}
	return e
}

func sbSubmitScore(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, tag := r.Xuid, c.Tag(r.Xuid)
	d := req.Sub("data")
	typ := req.Int("type")
	if !c.within(r, "score", scoreLimit) {
		c.LogEvery(r, fmt.Sprintf("score-limit/%d", xuid), 60, fmt.Sprintf("  SubmitScore user=%#x: over the rate limit, ignored (more than %d per %d s for this account)", xuid, scoreLimit[0][0], scoreLimit[0][1]), diag.Warn, "")
		return nil, nil
	}
	if r.OwnCars {
		c.Log(r, fmt.Sprintf("  SubmitScore board=(%d,%d): not stored, the player runs its own car database", d.Int("scoreboardType"), typ))
		return nil, nil
	}
	sub := &scoreboard.Submission{Score: uint64(d.Int("score")), ScoreboardType: d.Int("scoreboardType"), GameScoreboardID: d.Int("gameScoreboardId"), TrackID: d.Int("trackId"),
		CarID: d.Int("carId"), CarClassID: d.Int("carClassId"), CarPerformanceIndex: d.Float("carPerformanceIndex"), IsClean: d.Int("isClean") != 0, UsedSTM: d.Int("usedSTM") != 0,
		UsedTCS: d.Int("usedTCS") != 0, UsedABS: d.Int("usedABS") != 0, UsedAutoBrake: d.Int("usedAutoBrake") != 0, UsedAutoShifting: d.Int("usedAutoShifting") != 0,
		UsedClutch: d.Int("usedClutch") != 0}
	if v, ok := d.Vals["score"].(uint64); ok {
		sub.Score = v
	}
	s, err := c.Sb.Submit(xuid, tag, typ, sub)
	if err != nil {
		return nil, err
	}
	if s == nil {
		score := scoreboard.U64ToDbl(sub.Score)
		req := c.Sb.EventClass(sub.ScoreboardType, sub.GameScoreboardID)
		var why string
		switch {
		case scoreboard.EventBoards[sub.ScoreboardType] && (sub.CarClassID == 6 || sub.CarClassID == 7):
			why = "class X/R not allowed on a Rivals board"
		case req != nil && sub.CarClassID != *req:
			why = fmt.Sprintf("car class %d does not match event class %d", sub.CarClassID, *req)
		case !math.IsNaN(score) && !math.IsInf(score, 0):
			why = fmt.Sprintf("speed %.3f m/s is above the %s m/s cap", score, fmtG(scoreboard.SpeedCap))
		default:
			why = "score is not a finite number"
		}
		c.Log(r, fmt.Sprintf("  SubmitScore board=(%d,%d) track=%d: ignored, %s", sub.ScoreboardType, typ, sub.TrackID, why))
		return nil, nil
	}
	c.Log(r, fmt.Sprintf("  SubmitScore board=(%d,%d) track=%d car=%d score=%.3f clean=%s -> ghost upload for score %x", sub.ScoreboardType, typ, sub.TrackID, sub.CarID,
		s.Score, b2s(sub.IsClean), s.ScoreID[:4]))
	if c.Sb.GhostPolicy != "enforce" || !scoreboard.GhostBoards[sub.ScoreboardType] {
		func() {
			defer func() {
				if p := recover(); p != nil {
					c.LogExc(r, fmt.Sprintf("  notify_beaten failed after the score was stored (board %d, event %d); the submission stands, the rivals were not told", sub.ScoreboardType, sub.GameScoreboardID), fmt.Errorf("%v", p), diag.Warn)
				}
			}()
			if err := c.notifyBeaten(r, xuid, tag, typ, sub, s.Score); err != nil {
				c.LogExc(r, fmt.Sprintf("  notify_beaten failed after the score was stored (board %d, event %d); the submission stands, the rivals were not told", sub.ScoreboardType, sub.GameScoreboardID), err, diag.Warn)
			}
		}()
	}
	out := xls.NewStruct()
	out.Set("scoreId", s.ScoreID)
	out.Set("uploadFiles", true)
	return out, nil
}

// notifyBeaten: a new personal best on a Monthly event tells every other real player whose best on that event is slower: "<tag> beat you in the
// <event> challenge" (Message Center type 0x8403, shown under Rivals > My Rivals). One notification per (beaten player, beater, event): a newer one
// replaces the old, so repeat laps do not pile up.
func (c *Core) notifyBeaten(r *Req, xuid int64, tag string, scoreType int64, d *scoreboard.Submission, score float64) error {
	if d.ScoreboardType != 3 {
		return nil
	}
	e := c.eventInfo(d.GameScoreboardID)
	if e == nil {
		return nil
	}
	best, err := c.Sb.BestPerPlayer(3, scoreType, -1, e.ID)
	if err != nil {
		return err
	}
	var mine *scoreboard.Row
	for _, b := range best {
		if b.Xuid == xuid {
			mine = b
		}
	}
	if mine == nil || math.Abs(mine.Score-score) > 1e-9 {
		return nil // not the submitter's best on this event
	}
	for _, row := range best {
		if row.Xuid == xuid || !scoreboard.Better(scoreType, score, row.Score) {
			continue
		}
		if err := c.sendBeaten(r, row.Xuid, xuid, tag, e, d.CarID, d.CarClassID, scoreType, score); err != nil {
			return err
		}
	}
	return nil
}

// sendBeaten is Message Center 0x8403 to the beaten player; it replaces an older one from the same beater for the same event.
func (c *Core) sendBeaten(r *Req, beatenXuid, beaterXuid int64, beaterTag string, e *rivals.Event, carID, carClass, scoreType int64, score float64) error {
	vis, err := c.Notif.Visible(beatenXuid)
	if err != nil {
		return err
	}
	for _, n := range vis {
		if n.Type == 0x8403 {
			tt, _ := n.Params.Get("timeTrialId")
			sx, _ := n.Params.Get("senderXuid")
			if numEq(tt, e.ID) && numEq(sx, beaterXuid) {
				if _, err := c.Notif.Delete(n.ID); err != nil {
					return err
				}
			}
		}
	}
	tag := beaterTag
	if tag == "" {
		tag = "Rival"
	}
	params := pyjson.Obj{{K: "senderXuid", V: beaterXuid}, {K: "senderGamertag", V: cut(tag, 31)}, {K: "senderClubTag", V: ""}, {K: "senderClubRole", V: int64(0)},
		{K: "senderFlags", V: int64(0)}, {K: "timeTrialId", V: e.ID}, {K: "carId", V: carID}, {K: "carClassId", V: min(carClass, topClass)},
		{K: "localizedTimeTrialName", V: cut(e.Name, 127)}, {K: "trackName", V: cut(e.TrackName, 63)}, {K: "trackId", V: e.Track},
		{K: "score", V: scoreboard.DblToU64(score)}, {K: "scoreType", V: scoreType}, {K: "category", V: int64(monthlyCategory)}}
	if _, err := c.AddNotification(r, "", "", 0x8403, params, beatenXuid, nil, false); err != nil {
		return err
	}
	who := beaterTag
	if who == "" {
		who = fmt.Sprint(beaterXuid)
	}
	c.Log(r, fmt.Sprintf("  %s beat player %#x on event %d: told them (0x8403)", who, beatenXuid, e.ID))
	return nil
}

func numEq(v any, want int64) bool {
	switch x := v.(type) {
	case int64:
		return x == want
	case uint64:
		return int64(x) == want
	case float64:
		return x == float64(want)
	}
	return false
}

// sbNotifyRival: the client says it beat rivalXuid in event rivalEventId with score myScoreId (no result): that rival gets the "beat you" message, as in
// retail [inferred: the call is made when a real rival is beaten]. Made-up rivals have xuid 0 and are ignored.
func sbNotifyRival(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, tag := r.Xuid, c.Tag(r.Xuid)
	sub, err := c.Sb.ByScoreID(req.Bytes("myScoreId"))
	if err != nil {
		return nil, err
	}
	rivalEvent, rivalXuid := req.Int("rivalEventId"), req.Int("rivalXuid")
	e := c.eventInfo(rivalEvent)
	sc := "?"
	if sub != nil {
		sc = fmt.Sprintf("%.3f", sub.Score)
	}
	c.Log(r, fmt.Sprintf("  NotifyRival event=%d rival=%#x score=%s", rivalEvent, rivalXuid, sc))
	if sub == nil || e == nil || rivalXuid == 0 || rivalXuid == xuid || c.Sb.FakeName(rivalXuid) != "" {
		return nil, nil
	}
	vis, err := c.Sb.IsVisible(sub.ID)
	if err != nil {
		return nil, err
	}
	if sub.Xuid != xuid || sub.Gsid != e.ID || !vis {
		return nil, nil // only your own, public run on that event
	}
	theirs, err := c.Sb.BestOf(rivalXuid, sub.Sb, sub.St, -1, e.ID)
	if err != nil {
		return nil, err
	}
	if theirs == nil || !scoreboard.Better(sub.St, sub.Score, theirs.Score) {
		return nil, nil
	}
	if tag == "" {
		tag = sub.Tag
	}
	return nil, c.sendBeaten(r, rivalXuid, xuid, tag, e, sub.Car, sub.Cls, sub.St, sub.Score)
}

// announceRotation: once per rotation, a Rival Recommendation (0x8402) to everyone for one of the new events (Message Center / My Rivals).
func (c *Core) announceRotation(r *Req) error {
	if c.Riv == nil {
		return nil
	}
	month, n, err := c.Riv.Serial(nil)
	if err != nil {
		return err
	}
	key := fmt.Sprintf("%d:%d", month, n)
	row, err := c.DB.One("SELECT value FROM meta WHERE key='rival_announced'")
	if err != nil {
		return err
	}
	if row != nil && row.Str("value") == key {
		return nil
	}
	evs, err := c.Riv.Events()
	if err != nil || len(evs) == 0 {
		return err
	}
	e := evs[(month*7+n*3)%int64(len(evs))]
	params := pyjson.Obj{{K: "TimeTrialId", V: e.ID}, {K: "localizedTimeTrialName", V: cut(e.Name, 127)}, {K: "trackName", V: cut(e.TrackName, 63)},
		{K: "description", V: cut(e.Description, 511)}, {K: "category", V: int64(monthlyCategory)}}
	if _, err := c.AddNotification(r, "", "", 0x8402, params, 0, nil, false); err != nil {
		return err
	}
	err = c.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("INSERT INTO meta (key, value) VALUES ('rival_announced', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", key)
		return err
	})
	c.Log(r, fmt.Sprintf("  announced rotation %s: recommended event %d (%s)", key, e.ID, e.Name))
	return err
}

func sbUploadFile(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, tag := r.Xuid, c.Tag(r.Xuid)
	scoreID, data := req.Bytes("scoreId"), req.Bytes("data")
	sid, public, err := c.Sb.AttachGhost(scoreID, data, xuid)
	if err != nil {
		return nil, err
	}
	what := "refused (the scores: line above says why), nothing stored"
	if sid != 0 {
		what = fmt.Sprintf("ghost #%d", sid)
	}
	id8 := fmt.Sprintf("%x", scoreID)
	if len(id8) > 8 {
		id8 = id8[:8]
	}
	c.Log(r, fmt.Sprintf("  UploadFile scoreId=%s %d bytes -> %s user=%#x", id8, len(data), what, xuid))
	if public && c.Sb.GhostPolicy == "enforce" {
		sub, err := c.Sb.ByScoreID(scoreID)
		if err != nil {
			return nil, err
		}
		if sub != nil {
			d := &scoreboard.Submission{ScoreboardType: sub.Sb, GameScoreboardID: sub.Gsid, CarID: sub.Car, CarClassID: sub.Cls}
			if err := c.notifyBeaten(r, xuid, tag, sub.St, d, sub.Score); err != nil {
				c.LogExc(r, fmt.Sprintf("  notify_beaten failed after the ghost was stored (score #%d); the upload stands, the rivals were not told", sub.ID), err, diag.Warn)
			}
		}
	}
	return nil, nil
}

func sbDownloadFile(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	id, maxSize := req.Int("id"), req.Int("maxSize")
	var data []byte
	if id > 0 {
		var err error
		if data, err = c.Sb.PublicGhost(id); err != nil {
			return nil, err
		}
	}
	state := "no ghost"
	if data != nil {
		state = fmt.Sprint(len(data))
	}
	extra := ""
	if len(data) > 0 && int64(len(data)) > maxSize {
		extra = " (over maxSize, an empty file is sent)"
	}
	c.Log(r, fmt.Sprintf("  DownloadFile id=%d maxSize=%d: %s%s", id, maxSize, state, extra))
	out := xls.NewStruct()
	if data == nil || int64(len(data)) > maxSize {
		out.Set("data", []byte{})
	} else {
		out.Set("data", data)
	}
	return out, nil
}

func sbSubmitXP(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xp := req.Float("xp")
	if r.OwnCars {
		c.Log(r, fmt.Sprintf("  SubmitXpScore +%s: not stored, the player runs its own car database", fmtG(xp)))
		return nil, nil
	}
	total, verdict, reason, err := c.Sb.AddXP(r.Xuid, xp)
	if err != nil {
		return nil, err
	}
	if verdict == "ok" {
		c.Log(r, fmt.Sprintf("  SubmitXpScore +%s -> total %s user=%#x", fmtG(xp), fmtG(total), r.Xuid))
	} else {
		c.Log(r, fmt.Sprintf("  SubmitXpScore +%s REJECTED (%s), total stays %s user=%#x", fmtG(xp), reason, fmtG(total), r.Xuid))
	}
	return nil, nil
}

func sbBest(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	sbType, scoreType := req.Int("scoreboardType"), req.Int("scoreType")
	// gameScoreboardId is -1 on the class boards: the best across all classes would hide every slower run in another class
	pivot := req.Int("gameScoreboardId")
	if scoreboard.ClassPivot[sbType] {
		pivot = req.Int("carClassId")
	}
	b, err := c.Sb.BestOf(r.Xuid, sbType, scoreType, req.Int("trackId"), pivot)
	if err != nil {
		return nil, err
	}
	best := "none"
	if b != nil {
		best = fmt.Sprintf("%.3f", b.Score)
	}
	c.Log(r, fmt.Sprintf("  GetBestScore board=(%d,%d) gsid=%d track=%d car=%d: %s", sbType, scoreType, req.Int("gameScoreboardId"), req.Int("trackId"), req.Int("carId"), best))
	out, bs := xls.NewStruct(), xls.NewStruct()
	if b == nil {
		bs.Set("score", uint64(0))
		bs.Set("isClean", false)
		out.Set("bestScore", bs)
		out.Set("scoreFound", false)
		return out, nil
	}
	bs.Set("score", scoreboard.DblToU64(b.Score))
	bs.Set("isClean", b.Clean)
	out.Set("bestScore", bs)
	out.Set("scoreFound", true)
	return out, nil
}

func (c *Core) bountyTrack(req *xls.Struct) int64 {
	if e := c.eventInfo(req.Int("rivalEventId")); e != nil {
		return e.Track
	}
	return req.Int("trackId")
}

func sbRival(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	sbType, scoreType, track, pivot := req.Int("scoreboardType"), req.Int("scoreType"), req.Int("trackId"), req.Int("pivotId")
	row, err := c.Sb.RivalFor(r.Xuid, sbType, scoreType, track, pivot, req.Int("rivalXuid"))
	if err != nil {
		return nil, err
	}
	res := c.defCall("ForzaWebServicesScoreboardGetRival_Results")
	head := fmt.Sprintf("  GetRival board=(%d,%d) event=%d track=%d pivot=%d rivalXuid=%#x", sbType, scoreType, req.Int("rivalEventId"), track, pivot, req.Int("rivalXuid"))
	if row == nil {
		c.Log(r, head+": no rival")
		return res, nil
	}
	rival := res.Sub("rival")
	sd := rival.Sub("scoreData")
	c.scoreRow(row, sd, nil)
	bounty := int64(0)
	// your own ghost (you are #1, or picked yourself) and the made-up time of an empty board pay nothing: no credits to farm
	if row.Xuid != r.Xuid && !row.Fake {
		bounty = c.Sb.BountyFor(c.bountyTrack(req), row.Position)
	}
	sd.Set("bounty", bounty)
	rival.Set("bounty", bounty)
	extra := ""
	if row.Fake {
		extra = " (world record, empty board)"
	}
	c.Log(r, fmt.Sprintf("%s: %s %.3f%s", head, row.Tag, row.Score, extra))
	return res, nil
}

// clubBoardRow is a GetRows row of a club board: the club's leader as the user, the club's XP as the score, the club in clubData.
func (c *Core) clubBoardRow(rank int, clubID int64, xp float64, tmpl *xls.Struct) (*xls.Struct, error) {
	row := tmpl.Clone()
	club, err := c.Clubs.Club(clubID)
	if err != nil {
		return nil, err
	}
	leader, err := c.Clubs.Leader(clubID)
	if err != nil {
		return nil, err
	}
	if leader != 0 {
		row.Set("user", c.owner(tmpl.Sub("user"), leader))
	}
	row.Set("position", int64(rank))
	row.Set("rank", int64(rank))
	row.Set("score", scoreboard.DblToU64(xp))
	row.Set("submittedTime", scoreboard.Iso(c.Now()))
	members, err := c.Clubs.MemberCount(clubID)
	if err != nil {
		return nil, err
	}
	cd := row.Sub("clubData")
	cd.Set("clubId", clubID)
	cd.Set("clubTag", cut(club.Tag, 9))
	cd.Set("clubName", cut(club.Name, 49))
	cd.Set("memberCount", members)
	cd.Set("clubPrivacy", club.Privacy)
	return row, nil
}

// clubBoardRows serves the club leaderboards ("CLUB LEADERBOARDS / CLUB LEVEL"): GetRows with isClubBoard, one row per club by XP. view 0 = all clubs,
// 1 = my club, 2 = friends' clubs (none) [view numbers as seen on the player boards].
func (c *Core) clubBoardRows(r *Req, req *xls.Struct, results string) (*xls.Struct, error) {
	info, err := c.Clubs.MemberInfo(r.Xuid)
	if err != nil {
		return nil, err
	}
	mineID := info.ClubID
	lad, err := c.Clubs.Ladder()
	if err != nil {
		return nil, err
	}
	type entry struct {
		rank int
		id   int64
		xp   float64
	}
	var ladder []entry
	for i, l := range lad {
		ladder = append(ladder, entry{i + 1, l.ID, l.XP})
	}
	res := c.defCall(results)
	tmpl := res.Sub("userRow")
	view := req.Int("view")
	var page []entry
	switch view {
	case 1:
		for _, e := range ladder {
			if e.id == mineID {
				page = append(page, e)
			}
		}
	case 2:
	default:
		page = ladder
	}
	start := int(max(1, req.Int("startAt")))
	n := int(max(1, min(200, req.Int("maxResults"))))
	from := min(start-1, len(page))
	page = page[from:min(len(page), from+n)]
	var rows []any
	for _, e := range page {
		row, err := c.clubBoardRow(e.rank, e.id, e.xp, tmpl)
		if err != nil {
			return nil, err
		}
		rows = append(rows, row)
	}
	if rows == nil {
		rows = []any{}
	}
	res.Set("rows", rows)
	var mine *entry
	for i := range ladder {
		if ladder[i].id == mineID {
			mine = &ladder[i]
			break
		}
	}
	mineS := "-"
	if mine != nil {
		row, err := c.clubBoardRow(mine.rank, mine.id, mine.xp, tmpl)
		if err != nil {
			return nil, err
		}
		res.Set("userRow", row)
		mineS = fmt.Sprintf("#%d", mine.rank)
	}
	if view != 0 {
		res.Set("available", int64(len(page)))
	} else {
		res.Set("available", int64(len(ladder)))
	}
	c.Log(r, fmt.Sprintf("  GetRows[club board] board=(%d,%d) view=%d: %d clubs, mine=%s", req.Int("scoreboardType"), req.Int("scoreType"), view, len(rows), mineS))
	return res, nil
}

func sbRows(c *Core, r *Req, req *xls.Struct, results string) (*xls.Struct, error) {
	if req.Int("isClubBoard") != 0 && c.elemTemplate(results, "rows").Has("clubData") {
		return c.clubBoardRows(r, req, results)
	}
	sbType, scoreType, track, pivot := req.Int("scoreboardType"), req.Int("scoreType"), req.Int("trackId"), req.Int("pivotId")
	page, mine, nreal, nfake, err := c.Sb.Rows(r.Xuid, sbType, scoreType, track, int(max(1, req.Int("startAt"))), int(min(200, max(0, req.Int("maxResults")))), pivot)
	if err != nil {
		return nil, err
	}
	const friendsView = 2 // the leaderboard's "FILTER: FRIENDS"; we have no friends, so it is just you
	view := req.Int("view")
	if view == friendsView {
		if mine != nil {
			m := *mine
			m.Position = 1
			page, mine = []*scoreboard.Row{&m}, &m
		} else {
			page = nil
		}
	}
	res := c.defCall(results)
	tmpl := res.Sub("userRow")
	var xs []int64
	for _, p := range page {
		xs = append(xs, p.Xuid)
	}
	if mine != nil {
		xs = append(xs, mine.Xuid)
	}
	var infos map[int64]clubs.Info
	if c.Clubs != nil {
		if infos, err = c.Clubs.MemberInfos(xs); err != nil {
			return nil, err
		}
	}
	rows := []any{}
	for _, p := range page {
		rows = append(rows, c.scoreRow(p, tmpl.Clone(), infos))
	}
	res.Set("rows", rows)
	if mine != nil {
		res.Set("userRow", c.scoreRow(mine, res.Sub("userRow"), infos))
	}
	// `available` (GetRows) / `totalRowCount` (GetRowsAnthem) = size of the whole board. Left at 0 the leaderboard page shows "RESULTS UNAVAILABLE".
	var total int64
	switch {
	case view == friendsView && mine != nil:
		total = 1
	case view == friendsView:
		total = 0
	default:
		total = int64(nreal + nfake)
	}
	res.Set("available", total)
	if res.Has("totalRowCount") {
		res.Set("totalRowCount", total)
	}
	you := "-"
	if mine != nil {
		you = fmt.Sprintf("#%d", mine.Position)
	}
	name := strings.TrimSuffix(strings.TrimPrefix(results, "ForzaWebServicesScoreboard"), "_Results")
	c.Log(r, fmt.Sprintf("  GetRows[%s] board=(%d,%d) view=%d track=%d pivot=%d: %d rows (%d real, %d fake), you=%s", name, sbType, scoreType, view, track, pivot, len(rows), nreal, nfake, you))
	return res, nil
}

// ---------------------------------------------------------------- Monthly Rivals

// gameOptionsXML is `<GameOptions>` for a rival event in the game's own format. The confirmed effect so far is the lap count and the class limit.
// Other restriction attributes are named after the exe's option table [inferred, being tested]. The track is not part of it (GetTimeTrials.trackId).
func gameOptionsXML(e *rivals.Event) string {
	var attrs strings.Builder
	for _, p := range e.Attrs {
		fmt.Fprintf(&attrs, ` %s="%s"`, p.K, pyjson.PyStr(p.V))
	}
	buckets := ""
	if len(e.Attrs) > 0 {
		buckets = "<Buckets><Bucket><CarRestrictions" + attrs.String() + "/></Bucket></Buckets>"
	}
	tod := ""
	if e.Tod != nil {
		tod = fmt.Sprintf(` TimeOfDay="%d"`, rivals.TodSeconds(*e.Tod)) // seconds since midnight
	}
	weather := ""
	if e.Weather != nil {
		wet, pr := "None", "None"
		if e.Wetness != nil {
			wet = pyjson.PyRepr(*e.Wetness)
		}
		if e.Precip != nil {
			pr = pyjson.PyRepr(*e.Precip)
		}
		weather = `<DirectWeatherControlSettings ControlWeather="true" ControlAtmosphericPreset="true" ` +
			fmt.Sprintf(`AtmosphericPresetIndex="%d" ControlEnvironmentalWetness="true" DesiredEnvironmentalWetness="%s" ControlAtmosphericWetness="true" `+
				`DesiredAtmosphericWetness="%s" ControlPrecipitationLevel="true" DesiredPrecipitationLevel="%s"/>`, *e.Weather, wet, wet, pr)
	}
	return `<GameOptions UiRacificationMode="Race" DrivatarTrainingDisabled="false" UseOnlineAntiGriefing="true" ` +
		`UsePFXCheckpoints="true" PositionCPFlaresFromCentre="false" HasTraffic="false"` + tod + `>` +
		buckets + `<StartingGridOptions GridOrderingType="Random" Ascending="1"/>` +
		fmt.Sprintf(`<EndConditionOptions Type="NumLaps" NumLaps="%d"/>`, e.Laps) +
		`<ScoringOptions Type="TimeBased" TargetScoreEnabled="1" BiggerIsBetter="0" FinishingPlaceRelevant="true"/>` +
		weather + `</GameOptions>`
}

func hopperGameOptions(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	e := c.eventInfo(req.Int("hopperId"))
	out := xls.NewStruct()
	if e == nil {
		c.Log(r, fmt.Sprintf("  GetGameOptions hopper=%d round=%d: not a rival event, empty", req.Int("hopperId"), req.Int("roundNumber")))
		out.Set("gameOptionsXml", []byte{})
		return out, nil
	}
	xml := []byte(gameOptionsXML(e))
	c.Log(r, fmt.Sprintf("  GetGameOptions rival event %d (%s %s): %d lap(s) (%d bytes)", e.ID, e.Kind, pyjson.PyRepr(e.Attrs), e.Laps, len(xml)))
	out.Set("gameOptionsXml", xml[:min(int64(len(xml)), max(0, req.Int("optionXmlMaxLength")))])
	return out, nil
}

// sbTimeTrials lists the rival events under Rivals > Monthly. The race uses the event's trackId; laps and the car restriction come from
// Hopper.GetGameOptions, keyed on hopperId = event id.
func sbTimeTrials(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	if err := c.announceRotation(r); err != nil {
		return nil, err
	}
	now := c.Now()
	evs, err := c.Riv.Events()
	if err != nil {
		return nil, err
	}
	var tts []any
	for k, e := range evs {
		tt := xls.NewStruct()
		tt.Set("id", e.ID)
		tt.Set("category", int64(monthlyCategory))
		tt.Set("scoreType", int64(1))
		tt.Set("name", e.Name)
		tt.Set("description", e.Description)
		tt.Set("notRegisteredMessage", "")
		tt.Set("trackId", e.Track)
		tt.Set("trackName", e.TrackName)
		tt.Set("hopperId", e.ID)
		tt.Set("gameOptionsIndex", int64(0))
		tt.Set("startDate", scoreboard.Iso(now-86400))
		tt.Set("endDate", scoreboard.Iso(now+86400*30))
		tt.Set("isUserRegistered", true)
		tt.Set("isNew", k < 3)
		tt.Set("isFeatured", k == 0)
		tt.Set("isActive", true)
		tts = append(tts, tt)
	}
	n := req.Int("maxTimeTrials")
	c.Log(r, fmt.Sprintf("  GetTimeTrials max=%d: serving %d rival events", n, min(int(n), len(tts))))
	take := len(tts)
	if n > 0 {
		take = min(int(n), len(tts))
	}
	if tts == nil {
		tts = []any{}
	}
	out := xls.NewStruct()
	out.Set("timeTrials", tts[:take])
	return out, nil
}
