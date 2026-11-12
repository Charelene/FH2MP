package lsp

import (
	"fmt"
	"math"
	"os"
	"strconv"
	"time"

	"fh2mp/internal/achievements"
	"fh2mp/internal/auth"
	"fh2mp/internal/db"
	"fh2mp/internal/pyjson"
)

// A player's achievements as their Guide reported them (/v1/community/achievements, /v1/community/player/<xuid>/achievements). The client
// decides them (the Achievements mod); the server keeps what it was told so that other players' gamercards can show them. An unlock,
// once kept, stays: a report only adds.

// reportAchievements is POST /v1/community/achievements {"unlocked": {"<id>": "YYYY-MM-DD HH:MM:SS UTC", ...}}: the ids the list has
// are kept with their times, ids already kept keep the time they have. The answer is the player's count and Gamerscore, and "rarity":
// how many players report achievements and what share of them has each one (the Guide's rare toast).
func (l *Launcher) reportAchievements(me int64, body pyjson.Obj) (any, error) {
	v, _ := body.Get("unlocked")
	obj, isObj := v.(pyjson.Obj)
	if !isObj {
		return nil, auth.NewError(400, "bad_request", "").WithXuid(me).WithDetail("unlocked is " + pyjson.TypeName(v))
	}
	if len(obj) > 4*len(achievements.Defs()) {
		return nil, auth.NewError(400, "bad_request", "").WithXuid(me).WithDetail(fmt.Sprintf("%d unlocks reported, the game has %d achievements", len(obj), len(achievements.Defs())))
	}
	reported := make(map[string]string, len(obj))
	for _, p := range obj {
		when, _ := p.V.(string)
		reported[p.K] = when
	}
	kept, unknown := achievements.Accept(reported, time.Now().UTC())
	added := 0
	err := l.C.DB.Do(func(t *db.Tx) error {
		added = 0
		for _, u := range kept {
			n, err := t.Exec("INSERT INTO player_achievements (xuid, achievement, unlocked_at) VALUES (?,?,?) ON CONFLICT DO NOTHING", me, u.ID, u.At)
			if err != nil {
				return err
			}
			added += int(n.RowsAffected)
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	ids, _, err := l.achievementsOf(me)
	if err != nil {
		return nil, err
	}
	if added > 0 || unknown > 0 {
		communityLog.Info(fmt.Sprintf("achievements reported: xuid=%#x reported=%d new=%d unknown_ids=%d, now %d unlocked for %dG", me, len(obj), added, unknown,
			len(ids), achievements.Gamerscore(ids)))
	}
	rarity, err := l.achievementRarity()
	if err != nil {
		return nil, err
	}
	return pyjson.Obj{{K: "count", V: int64(len(ids))}, {K: "gamerscore", V: int64(achievements.Gamerscore(ids))}, {K: "rarity", V: rarity}}, nil
}

// RarePercent: an achievement fewer than this share of the players have is rare (Xbox Live's rule: under 10% of the game's players).
// RareMinPlayers: with fewer reporting players a share says nothing, and nothing is rare.
const (
	RarePercent    = 10.0
	RareMinPlayers = 10
)

// rarityOf turns the kept unlocks into shares: holders is achievement -> players who have it, players the number of players who report
// achievements (every player with the Guide and the Achievements mod reports its whole list). phantom is added to players: the dev stack's
// FH2MP_ACH_PHANTOM_PLAYERS, so two test accounts can see a rare toast; never set on prod.
func rarityOf(holders map[string]int64, players, phantom int64) pyjson.Obj {
	players += phantom
	percent := pyjson.Obj{}
	for _, d := range achievements.Defs() {
		share := 0.0
		if players > 0 {
			share = math.Round(float64(holders[d.ID])*1000/float64(players)) / 10
		}
		percent = append(percent, pyjson.Pair{K: d.ID, V: share})
	}
	return pyjson.Obj{{K: "players", V: players}, {K: "rareBelow", V: RarePercent}, {K: "minPlayers", V: int64(RareMinPlayers)}, {K: "percent", V: percent}}
}

func (l *Launcher) achievementRarity() (pyjson.Obj, error) {
	rows, err := l.C.DB.Query("SELECT achievement, COUNT(*) FROM player_achievements GROUP BY achievement")
	if err != nil {
		return nil, err
	}
	holders := map[string]int64{}
	for i := range rows {
		holders[rows[i].StrAt(0)] = rows[i].IntAt(1)
	}
	one, err := l.C.DB.Query("SELECT COUNT(DISTINCT xuid) FROM player_achievements")
	if err != nil {
		return nil, err
	}
	players := int64(0)
	if len(one) > 0 {
		players = one[0].IntAt(0)
	}
	phantom, _ := strconv.ParseInt(os.Getenv("FH2MP_ACH_PHANTOM_PLAYERS"), 10, 64)
	if phantom < 0 {
		phantom = 0
	}
	return rarityOf(holders, players, phantom), nil
}

// achievementsOf is a player's kept unlocks: the ids, and id -> when in the mod's form.
func (l *Launcher) achievementsOf(xuid int64) ([]string, pyjson.Obj, error) {
	rows, err := l.C.DB.Query("SELECT achievement, unlocked_at FROM player_achievements WHERE xuid=?", xuid)
	if err != nil {
		return nil, nil, err
	}
	at := make(map[string]float64, len(rows))
	for i := range rows {
		at[rows[i].Str("achievement")] = rows[i].Float("unlocked_at")
	}
	var ids []string
	when := pyjson.Obj{}
	for _, d := range achievements.Defs() { // the list's order
		if t, ok := at[d.ID]; ok {
			ids = append(ids, d.ID)
			when = append(when, pyjson.Pair{K: d.ID, V: achievements.When(t)})
		}
	}
	return ids, when, nil
}

// playerAchievements is GET /v1/community/player/<xuid>/achievements: what any player has unlocked, for the Guide's Compare Games.
func (l *Launcher) playerAchievements(me, target int64) (any, error) {
	row, err := l.C.DB.One("SELECT 1 FROM accounts WHERE xuid=? AND (banned_at IS NULL OR ban_until <= ?)", target, nowF())
	if err != nil {
		return nil, err
	}
	if row == nil {
		return nil, auth.NewError(404, "player_not_found", "No player has that gamertag or ID.").WithXuid(me).WithDetail(fmt.Sprintf("no unbanned account %#x", target))
	}
	ids, when, err := l.achievementsOf(target)
	if err != nil {
		return nil, err
	}
	return pyjson.Obj{{K: "xuid", V: target}, {K: "unlocked", V: when}, {K: "count", V: int64(len(ids))}, {K: "gamerscore", V: int64(achievements.Gamerscore(ids))}}, nil
}

// gamerscores is the Gamerscore of each of the players who have reported an unlock (the others are not in the map: a card says nothing
// about a player whose client never reported).
func (l *Launcher) gamerscores(xuids []int64) (map[int64]int64, error) {
	out := map[int64]int64{}
	if len(xuids) == 0 {
		return out, nil
	}
	rows, err := l.C.DB.Query("SELECT xuid, achievement FROM player_achievements WHERE xuid IN ("+marks(len(xuids))+")", int64Args(xuids)...)
	if err != nil {
		return nil, err
	}
	ids := map[int64][]string{}
	for i := range rows {
		x := rows[i].Int("xuid")
		ids[x] = append(ids[x], rows[i].Str("achievement"))
	}
	for x, list := range ids {
		out[x] = int64(achievements.Gamerscore(list))
	}
	return out, nil
}
