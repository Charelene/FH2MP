package clubs

import (
	"encoding/json"
	"errors"
	"os"
	"reflect"
	"sort"
	"testing"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
)

type fixture struct {
	Keywords  []map[string]any `json:"keywords"`
	Roles     [][3]int64       `json:"roles"`
	WeekStart [][2]float64     `json:"week_start"`
	Steps     []struct {
		S         string          `json:"s"`
		R         json.RawMessage `json:"r"`
		ClubError *[2]any         `json:"club_error"`
	} `json:"steps"`
}

func load(t *testing.T) *fixture {
	raw, err := os.ReadFile("../../testdata/clubs.json")
	if err != nil {
		t.Skip(err)
	}
	var f fixture
	if err := json.Unmarshal(raw, &f); err != nil {
		t.Fatal(err)
	}
	return &f
}

func TestPure(t *testing.T) {
	f := load(t)
	var kw []any
	for _, k := range KeywordList() {
		kw = append(kw, map[string]any{"Keyword": k.Keyword, "KeywordId": k.KeywordID, "Category": k.Category})
	}
	b, _ := json.Marshal(kw)
	var got, want any
	json.Unmarshal(b, &got)
	b, _ = json.Marshal(f.Keywords)
	json.Unmarshal(b, &want)
	if !reflect.DeepEqual(got, want) {
		t.Errorf("keywords differ")
	}
	for _, r := range f.Roles {
		if WireRole(r[0]) != r[1] || PowerRole(r[0]) != r[2] {
			t.Errorf("roles of %d: %d %d, python %d %d", r[0], WireRole(r[0]), PowerRole(r[0]), r[1], r[2])
		}
	}
	for _, w := range f.WeekStart {
		now := w[0]
		if got := WeekStart(&now); got != w[1] {
			t.Errorf("WeekStart(%v) = %v, python %v", w[0], got, w[1])
		}
	}
}

func cd(c *Club, members bool) any {
	if c == nil {
		return nil
	}
	m := map[string]any{"id": c.ID, "tag": c.Tag, "name": c.Name, "description": c.Description, "privacy": c.Privacy, "region": c.Region, "attitude": c.Attitude, "interest": c.Interest, "created": c.Created}
	if members {
		m["members"] = c.Members
	}
	return m
}

func clist(cs []*Club) any {
	out := []any{}
	for _, c := range cs {
		out = append(out, cd(c, true))
	}
	return out
}

func memberMaps(ms []Member) any {
	out := []any{}
	for _, m := range ms {
		out = append(out, map[string]any{"xuid": m.Xuid, "role": m.Role, "status_changed": m.StatusChanged})
	}
	return out
}

func standings(s []Standing) any {
	out := []any{}
	for _, x := range s {
		out = append(out, []any{x.ID, x.XP})
	}
	return out
}

func TestMatchesPython(t *testing.T) {
	f := load(t)
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	clock := 1_700_000_000.0
	old := Now
	Now = func() float64 { return clock }
	defer func() { Now = old }()
	for x := int64(1); x <= 12; x++ {
		if _, err := d.Exec("INSERT INTO players (xuid, gamertag, first_seen, last_seen) VALUES (?,?,?,?)", x, "P"+string(rune('0'+x/10))+string(rune('0'+x%10)), 1.0, 1.0); err != nil {
			t.Fatal(err)
		}
	}
	// Python's gamertags are f"P{x}" without padding
	d.Exec("UPDATE players SET gamertag = 'P' || xuid")
	for _, p := range []struct {
		x  int64
		xp float64
	}{{1, 5000}, {2, 12000}, {3, 700}, {4, 100}, {5, 30000}, {6, 40}, {7, 9000}} {
		if _, err := d.Exec("INSERT INTO player_xp (xuid, xp, updated) VALUES (?,?,?)", p.x, p.xp, 1.0); err != nil {
			t.Fatal(err)
		}
	}
	s := New(d)
	n := 0
	check := func(name string, got any, err error) {
		t.Helper()
		if n >= len(f.Steps) {
			t.Fatalf("%s: no more steps", name)
		}
		want := f.Steps[n]
		n++
		if want.S != name {
			t.Fatalf("step %d is %s in Python and %s in Go", n, want.S, name)
		}
		var ce *Error
		if errors.As(err, &ce) {
			if want.ClubError == nil || want.ClubError[0] != ce.Text || int(want.ClubError[1].(float64)) != ce.Code {
				t.Fatalf("%s: club error %q %d, python %v / %s", name, ce.Text, ce.Code, want.ClubError, want.R)
			}
			return
		}
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if want.ClubError != nil {
			t.Fatalf("%s: python raised %v, Go did not", name, want.ClubError)
		}
		b, _ := json.Marshal(got)
		var g, w any
		json.Unmarshal(b, &g)
		json.Unmarshal(want.R, &w)
		if !reflect.DeepEqual(g, w) {
			t.Fatalf("step %s differs\n got %s\nwant %s", name, b, want.R)
		}
	}
	tick := func() { clock += 10 }
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	c64 := func(v int64) *int64 { return &v }
	str := func(x ...string) string { return x[0] }
	_ = str
	tick()
	_, err = s.Create(1, "ab", "Name", "", 1, 0, 0, 0)
	check("bad_tag_short", nil, err)
	_, err = s.Create(1, "abcde", "Name", "", 1, 0, 0, 0)
	check("bad_tag_long", nil, err)
	_, err = s.Create(1, "abc", "   ", "", 1, 0, 0, 0)
	check("no_name", nil, err)
	tick()
	a, err := s.Create(1, " ALP ", "Alpha Racers", "We race a lot", 1, 2, 3, 4)
	check("create_alpha", a, err)
	_, err = s.Create(1, "ZZZ", "Other", "", 1, 0, 0, 0)
	check("create_again", nil, err)
	_, err = s.Create(2, "alp", "Dupe", "", 1, 0, 0, 0)
	check("tag_exists", nil, err)
	tick()
	b, err := s.Create(2, "BRV", string(repeat('B', 80)), string(repeat('d', 500)), 2, 1, 0, 0)
	check("create_brv", b, err)
	tick()
	cc, err := s.Create(3, "CHG", "Charlie Group", "", 3, 0, 5, 6)
	check("create_chg", cc, err)
	ca, _ := s.Club(a)
	check("club_a", cd(ca, false), nil)
	cb, _ := s.Club(b)
	check("club_b_truncated", cd(cb, false), nil)
	cn, err := s.Club(999)
	check("club_none", cd(cn, false), err)
	t1, _ := s.ByTag("alp")
	t2, _ := s.ByTag("ALP")
	t3, _ := s.ByTag("nope")
	check("by_tag", []any{cd(t1, false), cd(t2, false), cd(t3, false)}, nil)
	var ms []any
	for _, x := range []int64{1, 2, 9} {
		c, r, err := s.Membership(x)
		must(err)
		var rv any
		if c != nil {
			rv = r
		}
		ms = append(ms, []any{cd(c, false), rv})
	}
	check("membership", ms, nil)
	i1, _ := s.MemberInfo(1)
	i9, _ := s.MemberInfo(9)
	check("member_info", []any{[]any{i1.ClubID, i1.Tag, i1.Role}, []any{i9.ClubID, i9.Tag, i9.Role}}, nil)
	mi := func(xs []int64) any {
		m, err := s.MemberInfos(xs)
		must(err)
		out := []any{}
		keys := []int64{}
		for k := range m {
			keys = append(keys, k)
		}
		sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
		for _, k := range keys {
			out = append(out, []any{k, []any{m[k].ClubID, m[k].Tag, m[k].Role}})
		}
		return out
	}
	check("member_infos", mi([]int64{1, 2, 3, 9, 1, 2}), nil)
	m0, _ := s.MemberInfos(nil)
	check("member_infos_empty", map[string]any{}, nil)
	_ = m0

	join := func(name string, x int64, id *int64, tag string) {
		c, ok, err := s.RequestJoin(x, id, tag)
		var got any
		if err == nil {
			got = []any{cd(c, false), ok}
		}
		check(name, got, err)
	}
	tick()
	join("join_open", 4, c64(a), "")
	tick()
	join("join_open_by_tag", 5, nil, "alp")
	join("join_missing", 6, c64(999), "")
	join("join_missing_tag", 6, nil, "nope")
	join("join_same", 4, c64(a), "")
	join("join_private", 6, c64(cc), "")
	tick()
	join("join_invite_only", 6, c64(b), "")
	join("join_invite_only_again", 6, c64(b), "")
	appr := func(name string, l, u int64) {
		c, err := s.Approve(l, u)
		var got any
		if err == nil {
			got = cd(c, false)
		}
		check(name, got, err)
	}
	appr2 := func(name string, l, u int64) {
		appr(name, l, u)
	}
	_ = appr2
	// Python records an error instead of a value for these
	{
		_, err := s.Approve(4, 6)
		check("approve_no_privilege", nil, err)
		_, err = s.Approve(9, 6)
		check("approve_not_member", nil, err)
		_, err = s.Approve(2, 7)
		check("approve_no_request", nil, err)
	}
	tick()
	appr("approve", 2, 6)
	appr("approve_stale", 2, 6)
	appr("approve_while_in_other_club", 1, 6)
	tick()
	_, _, err = s.Invite(4, 7)
	check("invite_member_cannot", nil, err)
	_, _, err = s.Invite(9, 7)
	check("invite_no_club", nil, err)
	must(s.SetAcceptsInvites(8, false))
	a8, _ := s.AcceptsInvites(8)
	a7, _ := s.AcceptsInvites(7)
	a99, _ := s.AcceptsInvites(99)
	check("accepts_8", []bool{a8, a7, a99}, nil)
	_, _, err = s.Invite(1, 8)
	check("invite_refuses", nil, err)
	_, _, err = s.Invite(1, 4)
	check("invite_in_club", nil, err)
	tick()
	inv := func(name string, sp, iv int64) {
		c, iid, err := s.Invite(sp, iv)
		var got any
		if err == nil {
			got = []any{cd(c, false), iid}
		}
		check(name, got, err)
	}
	inv("invite_ok", 1, 7)
	_, _, err = s.Invite(1, 7)
	check("invite_again", nil, err)
	_, err = s.AcceptInvite(7, b, 123)
	check("accept_wrong_club", nil, err)
	tick()
	c, err := s.AcceptInvite(7, a, 55)
	var got any
	if err == nil {
		got = cd(c, false)
	}
	check("accept_invite", got, err)
	must(s.SetAcceptsInvites(8, true))
	a8, _ = s.AcceptsInvites(8)
	check("accepts_8_again", a8, nil)
	tick()
	inv("invite_8", 1, 8)
	ml, err := s.Members(a, 50)
	check("members_a", memberMaps(ml), err)
	ld, err := s.Leaders(a, 20)
	check("leaders_a", ld, err)
	l1, _ := s.Leader(a)
	l2, _ := s.Leader(999)
	check("leader_a", []int64{l1, l2}, nil)
	c1, _ := s.MemberCount(a)
	c2, _ := s.MemberCount(b)
	c3, _ := s.MemberCount(999)
	check("member_count", []int64{c1, c2, c3}, nil)
	tick()
	_, _, err = s.ChangeRole(4, 5, 2)
	check("promote_by_member", nil, err)
	_, _, err = s.ChangeRole(1, 2, 2)
	check("promote_other_club", nil, err)
	_, _, err = s.ChangeRole(1, 9, 2)
	check("promote_not_member", nil, err)
	_, _, err = s.ChangeRole(4, 4, 2)
	check("promote_self_up", nil, err)
	role := func(name string, actor, target, nr int64) {
		c, r, err := s.ChangeRole(actor, target, nr)
		var got any
		if err == nil {
			got = []any{cd(c, false), r}
		}
		check(name, got, err)
	}
	tick()
	role("promote_co1", 1, 4, 2)
	tick()
	role("promote_co2", 1, 5, 2)
	tick()
	role("promote_co3", 1, 7, 2)
	tick()
	role("demote_self", 4, 4, 1)
	tick()
	role("promote_co_again", 1, 4, 2)
	tick()
	_, _, err = s.RequestJoin(8, c64(a), "")
	must(err)
	ml, err = s.Members(a, 50)
	check("members_a_after", memberMaps(ml), err)
	_, _, err = s.ChangeRole(1, 8, 2)
	check("promote_co_limit", nil, err)
	ml, err = s.Members(a, 2)
	check("members_limit", memberMaps(ml), err)
	tick()
	role("hand_over", 1, 4, 3)
	ld, err = s.Leaders(a, 20)
	check("leaders_after", ld, err)
	l1, err = s.Leader(a)
	check("leader_after", l1, err)
	_, err = s.ChangeDetails(1, "x", "y", 1, 0, 0, 0)
	check("change_details_not_leader", nil, err)
	_, err = s.ChangeDetails(9, "x", "y", 1, 0, 0, 0)
	check("change_details_none", nil, err)
	desc := ""
	for i := 0; i < 40; i++ {
		desc += "New description "
	}
	cid, err := s.ChangeDetails(4, "", desc, 2, 7, 6, 5)
	check("change_details", cid, err)
	ca, err = s.Club(a)
	check("club_a_changed", cd(ca, false), err)

	rm := func(name string, actor, target int64) {
		c, kind, ds, err := s.Remove(actor, target)
		var got any
		if err == nil {
			sort.Slice(ds, func(i, j int) bool { return ds[i] < ds[j] })
			got = []any{cd(c, false), kind, ds}
		}
		check(name, got, err)
	}
	_, _, _, err = s.Remove(1, 9)
	check("kick_not_member", nil, err)
	_, _, _, err = s.Remove(8, 7)
	check("kick_no_privilege", nil, err)
	_, _, _, err = s.Remove(1, 4)
	check("kick_higher", nil, err)
	tick()
	rm("kick_ok", 4, 8)
	tick()
	rm("quit", 5, 5)
	_, _, _, err = s.Remove(5, 5)
	check("quit_again", nil, err)

	n0, err := s.PostNotice(7, "hello", []int64{1, 2})
	check("notice_no_privilege", n0, err)
	tick()
	n1, err := s.PostNotice(4, "Welcome all", []int64{11, 22, 33})
	check("notice_post", n1, err)
	nt, err := s.Notice(n1)
	var nm any
	if nt != nil {
		nm = map[string]any{"id": nt.ID, "club_id": nt.ClubID, "sender": nt.Sender, "role": nt.Role, "message": nt.Message, "created": nt.Created, "notification_ids": nt.NotificationIDs}
	}
	check("notice_get", nm, err)
	nt, err = s.Notice(999)
	check("notice_get_none", nt, err)
	_, err = s.DeleteNotice(2, n1)
	check("notice_delete_other_club", nil, err)
	_, err = s.DeleteNotice(4, 999)
	check("notice_delete_missing", nil, err)
	ids, err := s.DeleteNotice(1, n1)
	check("notice_delete", ids, err)
	nt, err = s.Notice(n1)
	check("notice_gone", nt, err)

	tick()
	for x := int64(9); x <= 12; x++ {
		if x%2 == 1 {
			_, _, err = s.RequestJoin(x, c64(a), "")
		} else {
			_, _, err = s.RequestJoin(x, nil, "BRV")
		}
		must(err)
	}
	sr := func(q SearchSpec) any {
		cs, err := s.Search(q)
		must(err)
		return clist(cs)
	}
	check("search_all", sr(SearchSpec{Limit: 20}), nil)
	check("search_privacy", []any{sr(SearchSpec{Privacy: 0, Limit: 20}), sr(SearchSpec{Privacy: 1, Limit: 20}), sr(SearchSpec{Privacy: 2, Limit: 20}), sr(SearchSpec{Privacy: 3, Limit: 20}), sr(SearchSpec{Privacy: -1, Limit: 20})}, nil)
	check("search_keywords", []any{sr(SearchSpec{Region: 7, Limit: 20}), sr(SearchSpec{Attitude: 5, Limit: 20}), sr(SearchSpec{Interest: 6, Limit: 20}), sr(SearchSpec{Region: 7, Attitude: 6, Interest: 5, Limit: 20})}, nil)
	check("search_name", []any{sr(SearchSpec{Name: "alpha", Limit: 20}), sr(SearchSpec{Name: "%", Limit: 20}), sr(SearchSpec{Name: "zzz", Limit: 20})}, nil)
	check("search_limit", []any{sr(SearchSpec{Limit: 1}), sr(SearchSpec{Limit: 0}), sr(SearchSpec{Limit: -5})}, nil)
	var rec []any
	for _, x := range []int64{1, 2, 3, 99} {
		cs, err := s.Recommended(x, 10)
		must(err)
		rec = append(rec, clist(cs))
	}
	check("recommended", rec, nil)
	cs, err := s.Recommended(99, 1)
	check("recommended_limit", clist(cs), err)
	tot, err := s.Total()
	check("total", tot, err)
	xps := func(ids ...int64) []int64 {
		var out []int64
		for _, id := range ids {
			v, err := s.XP(id)
			must(err)
			out = append(out, v)
		}
		return out
	}
	rank := func(ids ...int64) []int64 {
		var out []int64
		for _, id := range ids {
			v, err := s.XPRank(id)
			must(err)
			out = append(out, v)
		}
		return out
	}
	check("xp", xps(a, b, cc, 999), nil)
	check("xp_rank", rank(a, b, cc, 999), nil)
	ch := func(ids ...int64) []int64 {
		var out []int64
		for _, id := range ids {
			v, err := s.Champion(id)
			must(err)
			out = append(out, v)
		}
		return out
	}
	check("champion", ch(a, b, 999), nil)
	_, err = d.Exec("UPDATE player_xp SET xp = xp + 2500 WHERE xuid=?", 7)
	must(err)
	_, err = d.Exec("INSERT INTO player_xp (xuid, xp, updated) VALUES (?,?,?)", 9, 99999.0, 1.0)
	must(err)
	check("xp_after", xps(a, b, cc), nil)
	check("champion_after", ch(a, b), nil)
	lad, err := s.Ladder()
	check("ladder", standings(lad), err)
	now := clock
	ws := WeekStart(&now)
	for _, r := range []struct {
		x       int64
		xp, at  float64
		verdict string
	}{{7, 100, ws + 10, "ok"}, {7, 250, ws - 5, "ok"}, {7, 900, ws + 20, "rejected"}, {9, 60, ws + 30, "ok"}, {1, 500, ws + 5, "ok"}, {4, 70, ws + 40, "ok"}, {10, 33, ws + 50, "ok"}, {11, 5, 0, "ok"}} {
		_, err := d.Exec("INSERT INTO xp_awards (xuid, xp, at, verdict, reason) VALUES (?,?,?,?,?)", r.x, r.xp, r.at, r.verdict, "")
		must(err)
	}
	ladder := func(name string, id int64, at *float64) {
		l, err := s.MemberLadder(id, at)
		check(name, standings(l), err)
	}
	ladder("member_ladder_a", a, &now)
	ladder("member_ladder_b", b, &now)
	ladder("member_ladder_none", 999, &now)
	later := now + 14*86400
	ladder("member_ladder_later", a, &later)
	clock = now
	ladder("member_ladder_clock", a, nil)

	tick()
	join("leader_joins_other", 3, c64(a), "")
	cg, err := s.Club(cc)
	check("chg_gone", cd(cg, false), err)
	tick()
	rm("leader_quits", 2, 2)
	cb, _ = s.Club(b)
	mc, _, _ := s.Membership(6)
	i6, _ := s.MemberInfo(6)
	check("brv_gone", []any{cd(cb, false), cd(mc, false), []any{i6.ClubID, i6.Tag, i6.Role}}, nil)

	dump := func(name, sql string) {
		rows, err := d.Query(sql)
		must(err)
		out := []any{}
		for i := range rows {
			row := []any{}
			for _, v := range rows[i].Vals {
				row = append(row, v)
			}
			out = append(out, row)
		}
		check(name, out, nil)
	}
	dump("history", "SELECT xuid, club_id, club_name, action, role, created FROM club_history ORDER BY id")
	dump("requests_left", "SELECT club_id, xuid, created FROM club_requests ORDER BY club_id, xuid")
	dump("invites_left", "SELECT club_id, sponsor, invitee, created FROM club_invites ORDER BY id")
	dump("members_final", "SELECT xuid, club_id, role, joined, status_changed, xp_base FROM club_members ORDER BY xuid")
	if n != len(f.Steps) {
		t.Fatalf("Go ran %d steps, Python %d", n, len(f.Steps))
	}
}

func repeat(c byte, n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = c
	}
	return b
}
