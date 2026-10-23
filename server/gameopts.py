"""Reads what the leader chose in the event options (Nerd::GameOptionsSynced, the transfer object a Free Roam leader writes when an event starts).

The object is a stream of [variable id: 10 bits][value] pairs with ascending ids (670 variables, registration order of the constructors:
tools/gameopts_schema.py). Only the variables the server needs are read here; the rest is passed on untouched.

NumTeams (ScoringOptions, 2 bits, 0..2) is variable 428 and TeamScoringType (2 bits) is 429, right behind it. Found 2026-10-22 from the
game's constructors and checked on the logs: every Road Trip team event of the dev server (7 of 7) and 25 Free Roam payloads on prod carry
[428][2][429]; random bits match it about once in four million offsets (a 22-bit signature).
"""

NUM_TEAMS_ID = 428
TEAM_SCORING_ID = 429
ID_BITS = 10


def num_teams(size, payload):
    """NumTeams the leader chose (2 = "Teams: On"), or None when the options do not carry it (the default, 0, is not written).
    Needs the TeamScoringType entry behind it, which the game always writes with it (a lone [428][2] is a 12-bit pattern that occurs by
    chance in about one payload in seven)."""
    mask = (1 << ID_BITS) - 1
    for o in range(0, size - (2 * ID_BITS + 2) + 1):
        if (payload >> o) & mask == NUM_TEAMS_ID and (payload >> (o + ID_BITS + 2)) & mask == TEAM_SCORING_ID:
            return (payload >> (o + ID_BITS)) & 3
    return None


def teams_on(game_options):
    """`game_options` is the server's (size, payload) of the lobby's current options, or None."""
    if not game_options:
        return False
    n = num_teams(*game_options)
    return n == 2                       # the range is 0..2: 3 cannot be written
