"""The history the game server keeps of every client's objects and replays to players who join later.

A late joiner is sent every message of every other player's objects since that player joined. Player objects (router 1) collect thousands
of single-variable updates over a long session (prod 2026-10-22: one player's history had 1,333 messages, 16 of 40 thousand router-1 updates
in 16 hours were the XP counter alone), and joiners that received 1,200+ replayed messages died within 40 s three times as often (33 % against
10 %). Only the last value of a variable matters to somebody who was not there, so superseded single-variable updates are left out.

The receiving game applies the messages of one object strictly in sequence order (8-bit, 0..254), so a replay is numbered afresh from 0 and
the live messages that follow carry the joiner's own numbers: `shift` = how far the server's count is ahead of what that joiner was sent.
"""

SEQ_MOD = 0xFF

# router -> (variable id bits, {variable id: value bits}) of the variables whose updates are safe to supersede: state, not events.
# Router 1 = the player object (LobbyPlayer + PlayerChoices): 12 Ready, 16 InitialCarLoading, 19 PlayerStatus, 27 XP (float), 28, 32 HasInitialCarBeenChosen,
# 41 a 64-bit id that flips between xuids. Seen as single-variable updates in the dev captures (size = id bits + value bits).
SINGLE_VARS = {1: (8, {12: 1, 16: 1, 19: 4, 27: 32, 28: 32, 32: 1, 41: 64})}


def single_var(router, size, payload):
    """The variable id when `payload` (an update of `size` bits) is exactly one whitelisted variable, else None."""
    spec = SINGLE_VARS.get(router)
    if not spec:
        return None
    id_bits, widths = spec
    vid = payload & ((1 << id_bits) - 1)
    if widths.get(vid) is not None and size == id_bits + widths[vid]:
        return vid
    return None


def compact(router, hist, update_type):
    """`hist` = [(mtype, tid, seq, size, payload)] -> the same list without every single-variable update that a later single-variable
    update of the same variable supersedes. Creates and all other updates stay, in order."""
    if router not in SINGLE_VARS or len(hist) < 3:
        return list(hist)
    last = {}
    for i, (mtype, _tid, _seq, size, payload) in enumerate(hist):
        if mtype == update_type:
            vid = single_var(router, size, payload)
            if vid is not None:
                last[vid] = i
    out = []
    for i, e in enumerate(hist):
        if e[0] == update_type:
            vid = single_var(router, e[3], e[4])
            if vid is not None and last[vid] != i:
                continue
        out.append(e)
    return out


def renumber(entries):
    """The replay of one object for a joiner: [(mtype, tid, seq, size, payload)] with seq counting 0, 1, 2 ... (mod 255)."""
    return [(m, tid, i % SEQ_MOD, size, payload) for i, (m, tid, _s, size, payload) in enumerate(entries)]


def wire_seq(n, shift):
    """The sequence number a recipient is sent for the server's n-th message of an object (shift = server count - what that recipient got)."""
    return (n - shift) % SEQ_MOD
