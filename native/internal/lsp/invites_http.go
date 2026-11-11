package lsp

import (
	"fmt"
	"slices"

	"fh2mp/internal/auth"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/xbl"
)

// sendInvite is POST /v1/community/invites {"player": xuid or gamertag, "text": optional}.
func (l *Launcher) sendInvite(me int64, player string, textV any) (any, error) {
	target, err := l.resolve(player)
	if err != nil {
		return nil, err
	}
	text := ""
	if textV != nil {
		s, ok := textV.(string)
		if !ok {
			return nil, auth.NewError(400, "bad_message", "").WithXuid(me).WithDetail("text is " + pyjson.TypeName(textV))
		}
		text = s
	}
	if target == me {
		return nil, auth.NewError(400, "bad_request", "You cannot invite yourself.").WithXuid(me)
	}
	friends, err := l.C.Social.Friends(me)
	if err != nil {
		return nil, err
	}
	if !slices.Contains(friends, target) {
		return nil, auth.NewError(403, "not_friends", "You can only invite friends.").WithXuid(me).WithDetail(fmt.Sprintf("%#x is not a friend", target))
	}
	var lobbies []any
	if r := l.hubLobbies(); r != nil {
		lv, _ := r.Get("lobbies")
		lobbies = anyList(lv)
	}
	cards, err := l.profiles([]int64{me, target}, lobbies)
	if err != nil {
		return nil, err
	}
	mine, _ := cards[me].Get("lobby")
	lobby, _ := mine.(pyjson.Obj)
	if lobby == nil {
		return nil, auth.NewError(409, "not_in_game", "Start or join a game first, then invite your friends.").WithXuid(me).WithDetail("the player is in no lobby")
	}
	id, _ := lobby.Get("id")
	kind, _ := lobby.Get("kind")
	lobbyID, _ := id.(string)
	kindS, _ := kind.(string)
	if theirs, _ := cards[target].Get("lobby"); theirs != nil {
		if tl, _ := theirs.(pyjson.Obj); tl != nil {
			if tid, _ := tl.Get("id"); tid == id {
				return nil, auth.NewError(409, "already_here", "That friend is already in your game.").WithXuid(me)
			}
		}
	}
	iv, err := l.cstate().invites.Add(me, target, text, lobbyID, kindS)
	if err != nil {
		return nil, err
	}
	state, _ := cards[target].Get("state")
	communityLog.Info(fmt.Sprintf("game invite sent: from=%#x to=%#x (%v) lobby=%.8s kind=%s text=%d chars, pending until it is taken or %.0f s pass", me, target, state, lobbyID, kindS, len([]rune(iv.Text)), inviteKeep))
	return pyjson.Obj{{K: "ok", V: true}, {K: "id", V: iv.ID}}, nil
}

// listInvites is GET /v1/community/invites: the player's pending invites, newest first, each with its sender's card.
func (l *Launcher) listInvites(me int64) (any, error) {
	list := l.cstate().invites.List(me)
	senders := make([]int64, len(list))
	for i, iv := range list {
		senders[i] = iv.From
	}
	cards, err := l.profiles(senders, nil)
	if err != nil {
		return nil, err
	}
	now := nowF()
	out := []any{}
	for _, iv := range list {
		out = append(out, pyjson.Obj{{K: "id", V: iv.ID}, {K: "from", V: cards[iv.From]}, {K: "text", V: iv.Text}, {K: "kind", V: iv.Kind},
			{K: "age", V: int64(now - iv.Created)}, {K: "expiresIn", V: int64(inviteKeep - (now - iv.Created))}})
	}
	return pyjson.Obj{{K: "invites", V: out}}, nil
}

// acceptInvite is POST /v1/community/invites/<id>/accept: the seat lease in the inviter's lobby. The invite is used up only when the lease was made,
// so a full or vanished lobby leaves it to be declined or retried.
func (l *Launcher) acceptInvite(me, id int64) (any, error) {
	cs := l.cstate()
	iv, ok := cs.invites.Get(me, id)
	if !ok {
		return nil, auth.NewError(404, "invite_gone", "That invite has expired.").WithXuid(me)
	}
	h, err := l.C.Xbl.Reserve(fmt.Sprint(me), iv.Lobby, "")
	if err != nil {
		return nil, err
	}
	cs.invites.Take(me, id)
	communityLog.Info(fmt.Sprintf("game invite accepted: to=%#x from=%#x lobby=%.8s, seat leased for %d s", me, iv.From, iv.Lobby, xbl.HoldSeconds))
	return pyjson.Obj{{K: "reservation", V: publicHold(h)}}, nil
}
