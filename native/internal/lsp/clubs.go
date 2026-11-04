package lsp

import (
	"fmt"
	"time"

	"fh2mp/internal/clubs"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/scoreboard"
	"fh2mp/internal/xls"
)

// Car Clubs (package clubs).

func init() {
	const car, club, user = "Forza.WebServices.CarClub", "Forza.WebServices.Club", "Forza.WebServices.User"
	register(car, "GetClubDetails", clubCall("club_details", clubDetails))
	register(car, "GetClubKeywords", clubKeywords)
	register(car, "GetFriendsClubs", clubFriends)
	register(car, "GetRecommendedClubs", clubRecommended)
	register(car, "SearchForClubByTag", clubCall("club_search_tag", clubSearchTag))
	register(car, "SearchForClubs", clubSearch)
	register(car, "SearchForClubsByName", clubSearchName)
	// The Club* calls arrive with class Xls.WebServices.Club (game 2026-10-19); the Forza name is kept too.
	for _, cls := range []string{club, "Xls.WebServices.Club"} {
		register(cls, "CreateClub", clubCall("club_create", clubCreate))
		register(cls, "ChangeClubDetails", clubCall("club_change_details", clubChangeDetails))
		register(cls, "GetClubMembers", clubMembers)
		register(cls, "ChangeMemberRole", clubCall("club_change_role", clubChangeRole))
		register(cls, "InviteToClub", clubCall("club_invite", clubInvite))
		register(cls, "AcceptClubInvite", clubCall("club_accept_invite", clubAcceptInvite))
		register(cls, "RequestJoiningClub", clubCall("club_request_join", clubRequestJoin))
		register(cls, "ApproveRequestToJoin", clubCall("club_approve", clubApprove))
		register(cls, "RemoveFromClub", clubCall("club_remove", clubRemove))
		register(cls, "LeaveClub", clubCall("club_leave", clubLeave))
		register(cls, "CreateNoticeBoardNotification", clubCall("club_post_notice", clubPostNotice))
		register(cls, "DeleteNoticeBoardNotification", clubCall("club_delete_notice", clubDeleteNotice))
	}
	for _, cls := range []string{user, "Xls.WebServices.User"} {
		register(cls, "AcceptClubInvites", clubAcceptInvitesPref)
		register(cls, "GetUserData", userGetUserData)
	}
	register("Forza.WebServices.Scoreboard", "GetLadderRows", clubLadder)
}

// clubCall turns a clubs.Error into the Xls error result the client maps to one of its club messages.
func clubCall(name string, fn handlerFn) handlerFn {
	return func(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
		res, err := fn(c, r, req)
		if ce, ok := err.(*clubs.Error); ok {
			c.Log(r, fmt.Sprintf("  club: %s (error code %#x) call=%s user=%#x", ce.Text, ce.Code, name, r.Xuid))
			return nil, &XlsError{ce.Text, ce.Code}
		}
		return res, err
	}
}

func notFound() error { return &clubs.Error{Text: "club not found", Code: clubs.Codes["not_found"]} }

func (c *Core) keywordStruct(tmpl *xls.Struct, kid int64) *xls.Struct {
	t := tmpl.Clone()
	for _, k := range clubs.KeywordList() {
		if int64(k.KeywordID) == kid {
			t.Set("Keyword", k.Keyword)
			t.Set("KeywordId", int64(k.KeywordID))
			t.Set("Category", int64(k.Category))
		}
	}
	return t
}

// clubStruct is a club description in any of the result shapes (details, full info, search rows); fields a shape lacks are skipped.
func (c *Core) clubStruct(tmpl *xls.Struct, club *clubs.Club) (*xls.Struct, error) {
	cid := club.ID
	out := tmpl.Clone()
	user := func(x int64) *xls.Struct { return c.owner(tmpl.Sub("clubLeader"), x) }
	members, err := c.Clubs.MemberCount(cid)
	if err != nil {
		return nil, err
	}
	leader, err := c.Clubs.Leader(cid)
	if err != nil {
		return nil, err
	}
	champion, err := c.Clubs.Champion(cid)
	if err != nil {
		return nil, err
	}
	xp, err := c.Clubs.XP(cid)
	if err != nil {
		return nil, err
	}
	rank, err := c.Clubs.XPRank(cid)
	if err != nil {
		return nil, err
	}
	leaderV, champV := any(tmpl.Get("clubLeader")), any(tmpl.Get("clubChampion"))
	if leader != 0 {
		leaderV = user(leader)
	}
	if champion != 0 {
		champV = user(champion)
	}
	set := func(k string, v any) {
		if out.Has(k) {
			out.Set(k, v)
		}
	}
	set("clubId", cid)
	set("clubTag", cut(club.Tag, 9))
	set("clubName", cut(club.Name, 49))
	set("clubDescription", cut(club.Description, 400))
	set("isOpenForMembership", club.Privacy == clubs.PrivacyOpen)
	set("creationDate", iso(club.Created))
	set("carCount", int64(0))
	set("memberCount", members)
	set("clubLeader", leaderV)
	set("clubChampion", champV)
	set("clubPrivacy", club.Privacy)
	set("clubXP", floorDiv64(xp, c.ClubXPDivisor))
	set("clubXPRank", rank)
	for _, fc := range []struct {
		field string
		val   int64
	}{{"regionKeyword", club.Region}, {"attitudeKeyword", club.Attitude}, {"interestKeyword", club.Interest}} {
		if out.Has(fc.field) {
			out.Set(fc.field, c.keywordStruct(tmpl.Sub(fc.field), fc.val))
		}
	}
	return out, nil
}

func floorDiv64(a, b int64) int64 {
	q := a / b
	if a%b != 0 && (a < 0) != (b < 0) {
		q--
	}
	return q
}

func (c *Core) clubList(resultStruct, listField string, rows []*clubs.Club, totalField string) (*xls.Struct, error) {
	el := c.elemTemplate(resultStruct, listField)
	out := xls.NewStruct()
	list := []any{}
	for _, cl := range rows {
		s, err := c.clubStruct(el, cl)
		if err != nil {
			return nil, err
		}
		list = append(list, s)
	}
	out.Set(listField, list)
	for _, f := range c.callStruct(resultStruct) {
		if f.Name == totalField {
			n, err := c.Clubs.Total()
			if err != nil {
				return nil, err
			}
			out.Set(totalField, n)
		}
	}
	return out, nil
}

func clubDetails(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	club, err := c.Clubs.Club(req.Int("clubId"))
	if err != nil {
		return nil, err
	}
	if club == nil {
		return nil, notFound()
	}
	res := c.defCall("ForzaCarClubGetClubDetails_Results")
	det, err := c.clubStruct(res.Sub("clubDetails"), club)
	if err != nil {
		return nil, err
	}
	res.Set("clubDetails", det)
	return res, nil
}

func clubKeywords(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	all := clubs.KeywordList()
	n := int(max(1, req.Int("maxKeywordResults")))
	if len(all) > n {
		all = all[:n]
	}
	rows := []any{}
	for _, k := range all {
		s := xls.NewStruct()
		s.Set("Keyword", k.Keyword)
		s.Set("KeywordId", int64(k.KeywordID))
		s.Set("Category", int64(k.Category))
		rows = append(rows, s)
	}
	out := xls.NewStruct()
	out.Set("keywords", rows)
	return out, nil
}

func clubRecommended(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	rows, err := c.Clubs.Recommended(r.Xuid, int(req.Int("maxResults")))
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  GetRecommendedClubs: %d", len(rows)))
	return c.clubList("ForzaCarClubGetRecommendedClubs_Results", "clubDetailsList", rows, "totalClubs")
}

func clubFriends(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	return c.clubList("ForzaCarClubGetFriendsClubs_Results", "friendsClubDetailsList", nil, "totalClubs") // no friends list yet
}

// kw: the client sends 255 for "no keyword".
func kw(v int64) int64 {
	if v >= 255 {
		return 0
	}
	return v
}

func clubSearch(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	p := req.Sub("clubSearchParams")
	rows, err := c.Clubs.Search(clubs.SearchSpec{Privacy: p.Int("Privacy"), Region: kw(p.Int("RegionKeywordId")), Attitude: kw(p.Int("AttitudeKeywordId")),
		Interest: kw(p.Int("InterestKeywordId")), Limit: int(req.Int("maxResults"))})
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  SearchForClubs privacy=%d region=%d attitude=%d interest=%d activity=%d friends=%s: %d", p.Int("Privacy"), p.Int("RegionKeywordId"),
		p.Int("AttitudeKeywordId"), p.Int("InterestKeywordId"), p.Int("Activity"), pyBool(p.Get("FriendsOnly")), len(rows)))
	return c.clubList("ForzaCarClubSearchForClubs_Results", "clubDetailsList", rows, "totalClubs")
}

func pyBool(v any) string {
	switch x := v.(type) {
	case bool:
		if x {
			return "True"
		}
		return "False"
	case nil:
		return "None"
	}
	return fmt.Sprint(v)
}

func clubSearchName(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	rows, err := c.Clubs.Search(clubs.SearchSpec{Name: req.Str("clubName"), Limit: int(req.Int("maxResults"))})
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  SearchForClubsByName %s: %d", pyjson.PyRepr(req.Str("clubName")), len(rows)))
	return c.clubList("ForzaCarClubSearchForClubsByName_Results", "clubDetailsList", rows, "totalClubs")
}

func clubSearchTag(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	club, err := c.Clubs.ByTag(req.Str("clubTag"))
	if err != nil {
		return nil, err
	}
	if club == nil {
		return nil, notFound()
	}
	res := c.defCall("ForzaCarClubSearchForClubByTag_Results")
	det, err := c.clubStruct(res.Sub("clubDetails"), club)
	if err != nil {
		return nil, err
	}
	res.Set("clubDetails", det)
	return res, nil
}

func clubMembers(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	cid := req.Int("clubId")
	club, err := c.Clubs.Club(cid)
	if err != nil {
		return nil, err
	}
	el := c.elemTemplate("ClubGetClubMembers_Results", "clubMembers")
	var rows []clubs.Member
	var total int64
	if club != nil {
		if rows, err = c.Clubs.Members(cid, int(max(1, req.Int("maxMembers")))); err != nil {
			return nil, err
		}
		if total, err = c.Clubs.MemberCount(cid); err != nil {
			return nil, err
		}
	}
	out := []any{}
	for _, m := range rows {
		s := el.Clone()
		s.Set("user", c.owner(el.Sub("user"), m.Xuid))
		s.Set("recentClubStatusChange", c.Now()-m.StatusChanged < 86400)
		out = append(out, s)
	}
	res := xls.NewStruct()
	res.Set("clubMembers", out)
	res.Set("totalMembers", total)
	return res, nil
}

// me is the requesting player and the tag the server knows for them ("" when unknown).
func (c *Core) me(r *Req) (int64, string) { return r.Xuid, c.Tag(r.Xuid) }

func (c *Core) clubTagOf(xuid int64) string {
	t := c.Tag(xuid)
	if t == "" {
		t = c.tagFromDB(xuid)
	}
	return cut(t, 31)
}

func (c *Core) clubNotify(r *Req, xuid, ntype int64, params pyjson.Obj) (int64, error) {
	n, err := c.AddNotification(r, "", "", ntype, params, xuid, nil, false)
	if err != nil {
		return 0, err
	}
	return n.ID, nil
}

func (c *Core) notifyLeaders(r *Req, club *clubs.Club, ntype int64, params pyjson.Obj, exclude ...int64) error {
	leaders, err := c.Clubs.Leaders(club.ID, 50)
	if err != nil {
		return err
	}
	for _, x := range leaders {
		skip := false
		for _, e := range exclude {
			skip = skip || e == x
		}
		if !skip {
			if _, err := c.clubNotify(r, x, ntype, params); err != nil {
				return err
			}
		}
	}
	return nil
}

func nameTag(club *clubs.Club) pyjson.Obj {
	return pyjson.Obj{{K: "clubName", V: cut(club.Name, 49)}, {K: "clubTag", V: cut(club.Tag, 9)}}
}

func obj(kv ...any) pyjson.Obj {
	o := make(pyjson.Obj, 0, len(kv)/2)
	for i := 0; i+1 < len(kv); i += 2 {
		o = append(o, pyjson.Pair{K: kv[i].(string), V: kv[i+1]})
	}
	return o
}

func merge(a, b pyjson.Obj) pyjson.Obj { return append(append(pyjson.Obj(nil), a...), b...) }

func clubCreate(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	cid, err := c.Clubs.Create(xuid, req.Str("clubTag"), req.Str("clubName"), req.Str("clubDescription"), req.Int("privacy"), kw(req.Int("regionKeywordId")),
		kw(req.Int("attitudeKeywordId")), kw(req.Int("interestKeywordId")))
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  club created: #%d [%s] %s by %#x privacy=%d keywords=%d/%d/%d", cid, req.Str("clubTag"), pyjson.PyRepr(req.Str("clubName")), xuid, req.Int("privacy"),
		req.Int("regionKeywordId"), req.Int("attitudeKeywordId"), req.Int("interestKeywordId")))
	out := xls.NewStruct()
	out.Set("clubId", cid)
	return out, nil
}

func clubChangeDetails(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	_, err := c.Clubs.ChangeDetails(xuid, req.Str("newClubName"), req.Str("newClubDescription"), req.Int("privacyLevel"), kw(req.Int("regionKeyword")),
		kw(req.Int("attitudeKeyword")), kw(req.Int("interestKeyword")))
	return nil, err
}

// clearRequests: the player is in, their "club request" notices (0x700) in the leaders' Message Center are used up.
func (c *Core) clearRequests(club *clubs.Club, xuid int64) error {
	leaders, err := c.Clubs.Leaders(club.ID, 50)
	if err != nil {
		return err
	}
	for _, leader := range leaders {
		vis, err := c.Notif.Visible(leader)
		if err != nil {
			return err
		}
		for _, n := range vis {
			if v, _ := n.Params.Get("requestorXuid"); n.Type == 0x700 && numEq(v, xuid) {
				if _, err := c.Notif.Delete(n.ID); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

// joined: the leaders hear about the new member.
func (c *Core) joined(r *Req, club *clubs.Club, xuid int64) error {
	if err := c.clearRequests(club, xuid); err != nil {
		return err
	}
	return c.notifyLeaders(r, club, 0x707, obj("newMemberXuid", xuid, "newMemberGamertag", c.clubTagOf(xuid), "clubName", cut(club.Name, 49), "clubTag", cut(club.Tag, 9)), xuid)
}

func clubRequestJoin(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	id := req.Int("clubId")
	club, approved, err := c.Clubs.RequestJoin(xuid, &id, "")
	if err != nil {
		return nil, err
	}
	return c.afterRequest(r, club, xuid, approved)
}

func (c *Core) afterRequest(r *Req, club *clubs.Club, xuid int64, approved bool) (*xls.Struct, error) {
	var err error
	if approved {
		err = c.joined(r, club, xuid)
	} else {
		err = c.notifyLeaders(r, club, 0x700, obj("requestorXuid", xuid, "requestorGamertag", c.clubTagOf(xuid), "clubId", club.ID))
	}
	if err != nil {
		return nil, err
	}
	what := "request queued"
	if approved {
		what = "joined"
	}
	c.Log(r, fmt.Sprintf("  %#x asked to join [%s]: %s", xuid, club.Tag, what))
	out := xls.NewStruct()
	out.Set("requestApproved", approved)
	return out, nil
}

func clubApprove(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	user := req.Int("approvedUser")
	club, err := c.Clubs.Approve(xuid, user)
	if err != nil {
		return nil, err
	}
	if _, err := c.clubNotify(r, user, 0x705, nameTag(club)); err != nil {
		return nil, err
	}
	return nil, c.joined(r, club, user)
}

func (c *Core) invite(r *Req, club *clubs.Club, iid, sponsor, invitee int64) error {
	n, err := c.clubNotify(r, invitee, 0x701, obj("sponsorXuid", sponsor, "clubId", club.ID, "clubName", cut(club.Name, 49), "clubTag", cut(club.Tag, 9),
		"sponsorGamertag", c.clubTagOf(sponsor)))
	if err != nil {
		return err
	}
	c.Log(r, fmt.Sprintf("  club invite #%d [%s] %#x -> %#x (notification %d)", iid, club.Tag, sponsor, invitee, n))
	return nil
}

// LauncherClubInvite is the launcher's club chat invite: the same rules and the same Message Center notice as the game's own invite.
func (c *Core) LauncherClubInvite(sponsor, invitee int64) error {
	club, iid, err := c.Clubs.Invite(sponsor, invitee)
	if err != nil {
		return err
	}
	return c.invite(&Req{}, club, iid, sponsor, invitee)
}

func clubInvite(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	club, iid, err := c.Clubs.Invite(xuid, req.Int("userToInvite"))
	if err != nil {
		return nil, err
	}
	return nil, c.invite(r, club, iid, xuid, req.Int("userToInvite"))
}

func clubAcceptInvite(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	club, err := c.Clubs.AcceptInvite(xuid, req.Int("clubId"), req.Int("invitationId"))
	if err != nil {
		return nil, err
	}
	vis, err := c.Notif.Visible(xuid) // the invitation notices for this club are used up
	if err != nil {
		return nil, err
	}
	for _, n := range vis {
		if v, _ := n.Params.Get("clubId"); n.Type == 0x701 && numEq(v, club.ID) {
			if _, err := c.Notif.Delete(n.ID); err != nil {
				return nil, err
			}
		}
	}
	return nil, c.joined(r, club, xuid)
}

func clubChangeRole(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	target, newRole := req.Int("userToChange"), clubs.PowerRole(req.Int("newRole"))
	if newRole == 0 {
		return nil, &XlsError{"unknown role", clubs.Codes["unknown"]}
	}
	club, old, err := c.Clubs.ChangeRole(xuid, target, newRole)
	if err != nil {
		return nil, err
	}
	switch {
	case newRole > old:
		_, err = c.clubNotify(r, target, 0x708, merge(obj("newRole", clubs.WireRole(newRole), "newPromotedXuid", target, "newPromotedGamertag", c.clubTagOf(target)), nameTag(club)))
	case newRole < old:
		_, err = c.clubNotify(r, target, 0x704, nameTag(club))
	}
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  club role: %#x %d -> %d by %#x", target, old, newRole, xuid))
	return nil, nil
}

// clubRemoved tells the people concerned about a kick, a quit or a disbanded club, and logs it.
func (c *Core) clubRemoved(r *Req, xuid, target int64, club *clubs.Club, kind string, others []int64) error {
	var err error
	switch kind {
	case "kicked":
		_, err = c.clubNotify(r, target, 0x703, merge(obj("kickedXuid", target, "kickedGamertag", c.clubTagOf(target)), nameTag(club)))
	case "quit":
		err = c.notifyLeaders(r, club, 0x706, merge(obj("exMemberXuid", target, "exMemberGamertag", c.clubTagOf(target)), nameTag(club)))
	default:
		for _, x := range others { // the club was disbanded
			if _, err = c.clubNotify(r, x, 0x703, merge(obj("kickedXuid", x, "kickedGamertag", c.clubTagOf(x)), nameTag(club))); err != nil {
				break
			}
		}
	}
	if err != nil {
		return err
	}
	c.Log(r, fmt.Sprintf("  club remove: %#x %s by %#x from [%s]", target, kind, xuid, club.Tag))
	return nil
}

func clubRemove(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	target := req.Int("userToRemove")
	club, kind, others, err := c.Clubs.Remove(xuid, target)
	if err != nil {
		return nil, err
	}
	return nil, c.clubRemoved(r, xuid, target, club, kind, others)
}

// clubLeave is Xls.WebServices.Club.LeaveClub (no parameters): the player quits their own club; a Leader who leaves disbands it.
func clubLeave(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	club, kind, others, err := c.Clubs.Remove(xuid, xuid)
	if err != nil {
		return nil, err
	}
	return nil, c.clubRemoved(r, xuid, xuid, club, kind, others)
}

func clubPostNotice(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	club, role, err := c.Clubs.Membership(xuid)
	if err != nil {
		return nil, err
	}
	if club == nil || role < clubs.RoleCoLeader {
		return nil, &clubs.Error{Text: "insufficient privileges", Code: clubs.Codes["no_privilege"]}
	}
	msg := req.Str("noticeBoardMessage")
	params := obj("senderXuid", xuid, "senderClubRole", clubs.WireRole(role), "senderGamertag", c.clubTagOf(xuid), "noticeBoardMessage", cut(msg, 561))
	members, err := c.Clubs.Members(club.ID, 1000)
	if err != nil {
		return nil, err
	}
	ids := []int64{}
	for _, m := range members {
		id, err := c.clubNotify(r, m.Xuid, 0x709, params)
		if err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	nid, err := c.Clubs.PostNotice(xuid, msg, ids)
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  club notice #%d by %#x to %d members", nid, xuid, len(ids)))
	out := xls.NewStruct()
	out.Set("notificationId", nid)
	return out, nil
}

func clubDeleteNotice(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	ids, err := c.Clubs.DeleteNotice(xuid, req.Int("notificationId"))
	if err != nil {
		return nil, err
	}
	for _, nid := range ids {
		if _, err := c.Notif.Delete(nid); err != nil {
			return nil, err
		}
	}
	return nil, nil
}

func clubAcceptInvitesPref(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, _ := c.me(r)
	b, _ := req.Get("acceptingClubInvites").(bool)
	return nil, c.Clubs.SetAcceptsInvites(xuid, b)
}

// clubLadder is Scoreboard.GetLadderRows (Club Overview > View Club Ladder): the club's MEMBERS ranked by the XP they earned this week, the caller's own
// row and the reward tiers (Tier 1 = the highest threshold: the client fills the tier tabs by walking the list in order).
func clubLadder(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	const results = "ForzaWebServicesScoreboardGetLadderRows_Results"
	res := c.defCall(results)
	rowT, tierT := c.elemTemplate(results, "rows"), c.elemTemplate(results, "ladderTiers")
	xuid, _ := c.me(r)
	ladder, err := c.Clubs.MemberLadder(req.Int("clubId"), nil)
	if err != nil {
		return nil, err
	}
	top := float64(clubs.Tiers[0].XP)
	topCount := int64(0)
	for _, s := range ladder {
		if s.XP >= top {
			topCount++
		}
	}
	row := func(rank int64, member int64, xp float64) *xls.Struct {
		s := rowT.Clone()
		s.Set("rank", rank)
		s.Set("user", c.owner(rowT.Sub("user"), member))
		s.Set("score", scoreboard.DblToU64(xp))
		s.Set("topTierCount", topCount)
		return s
	}
	start := int(max(1, req.Int("startAt")))
	end := start - 1 + int(max(1, req.Int("maxResults")))
	rows := []any{}
	for i, s := range ladder {
		if i >= start-1 && i < end {
			rows = append(rows, row(int64(i+1), s.ID, s.XP))
		}
	}
	res.Set("rows", rows)
	tiers := []any{}
	for i, t := range clubs.Tiers {
		s := tierT.Clone()
		s.Set("xpThreshold", float64(t.XP))
		s.Set("creditRewardAmount", t.Credits)
		s.Set("tierNumber", int64(i+1))
		tiers = append(tiers, s)
	}
	if n := int(max(0, req.Int("maxTiers"))); len(tiers) > n {
		tiers = tiers[:n]
	}
	res.Set("ladderTiers", tiers)
	mine := "-"
	for i, s := range ladder {
		if s.ID == xuid {
			res.Set("userRow", row(int64(i+1), s.ID, s.XP))
			mine = fmt.Sprintf("#%d", i+1)
			break
		}
	}
	res.Set("available", int64(len(ladder)))
	c.Log(r, fmt.Sprintf("  GetLadderRows club=%d start=%d max=%d: %d of %d members, %d in Tier 1, you=%s", req.Int("clubId"), req.Int("startAt"), req.Int("maxResults"), len(rows),
		len(ladder), topCount, mine))
	return res, nil
}

// userGetUserData is Xls.WebServices.User.GetUserData: the client calls it after every club change and takes its own club tag / role from the answer.
func userGetUserData(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	xuid, tag := c.me(r)
	res := c.defCall("UserGetUserData_Results")
	u := c.owner(res.Sub("userData"), xuid)
	acc, err := c.Clubs.AcceptsInvites(xuid)
	if err != nil {
		return nil, err
	}
	u.Set("wzGamerTag", cut(tag, 31))
	u.Set("acceptsClubInvites", acc)
	res.Set("userData", u)
	return res, nil
}

var _ = time.Now
