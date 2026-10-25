"""Reads what the leader chose in the event options (Nerd::GameOptionsSynced, the transfer object a Free Roam leader writes when an event starts).

The object is a stream of [variable id: 10 bits][value] pairs with ascending ids (670 variables, registration order of the constructors:
tools/gameopts_schema.py). Only the variables the server needs are read here; the rest is passed on untouched.

NumTeams (ScoringOptions, 2 bits, 0..2) is variable 428 and TeamScoringType (3 bits) is 429, right behind it. Found 2026-10-22 from the
game's constructors and checked on the logs: every Road Trip team event of the dev server (7 of 7) and 25 Free Roam payloads on prod carry
[428][2][429]; random bits match it about once in four million offsets (a 22-bit signature).
"""

NUM_TEAMS_ID = 428
TEAM_SCORING_ID = 429
ID_BITS = 10
NUM_LAPS_ID = 419
END_CONDITION_ID = 418
MAX_LAPS = 10

# Constructor order through NumLaps, exe 1.0.0.64 (RVAs 0x17d4630,
# 0x17f93a0, 0x17ed620, 0x17fbe90, 0x17fdb90, 0x1818dd0, 0x1816700).
# Enum template arguments specify the bit width directly. A bucket name is a
# 5-bit length plus UTF-16 chars (0x17d8050); each restriction list is 50 u16s
# (0x17ef6f0). Parse from the beginning: scanning for a 10-bit id inside an
# arbitrary value could corrupt a track, car restriction or another setting.
_BUCKET_WIDTHS = (
    -24, 32, 1, 8, 8, 32, 32, 32, 8, 16, 16, 32, 32, 32, 32, 32, 32,
    2, 8, 2, 8, 2, 8, 2, 8, 2, 16, 2, 16, 2, 16, 2, 8, 2, 8, 2, 8,
    2, 8, 2, 8, 2, 8, 1, 32, 800, 800,
)
_LAP_PREFIX_WIDTHS = (
    8, 8, 3, 4, 32, 32, 8, 32, 32, 8, 32, 32, 4, 4, 8, 3, 2, 3, 1, 3, 8, 8,
) + _BUCKET_WIDTHS * 8 + (
    32, 32, 2, 3, 1, 1, 1, 1, 8, 8, 32, 32, 32, 1, 8, 8, 8, 32, 32, 32, 3, 32,
)


def _lap_fields(size, payload):
    """Return offsets/values for NumLaps and the end-condition type.

    Only the known prefix is decoded; the suffix is preserved byte for byte.
    Malformed/truncated prefixes raise ValueError rather than guessing a field.
    """
    pos, previous = 0, -1
    end_offset, end_type = None, None
    while pos < size:
        start = pos
        if pos + ID_BITS > size:
            raise ValueError("truncated GameOptions variable id")
        vid = (payload >> pos) & 1023
        pos += ID_BITS
        if vid <= previous or vid >= 670:
            raise ValueError("invalid GameOptions variable order")
        if vid > END_CONDITION_ID and end_offset is None:
            end_offset = start
        if vid > NUM_LAPS_ID:
            return start, None, end_offset, end_type
        width = _LAP_PREFIX_WIDTHS[vid]
        if width < 0:
            if pos + 5 > size:
                raise ValueError("truncated GameOptions bucket name")
            count = (payload >> pos) & 31
            if count > -width:
                raise ValueError("oversize GameOptions bucket name")
            width = 5 + 16 * count
        if pos + width > size:
            raise ValueError("truncated GameOptions variable")
        if vid == END_CONDITION_ID:
            end_offset, end_type = start, (payload >> pos) & 7
        if vid == NUM_LAPS_ID:
            return start, (payload >> pos) & 0xFFFFFFFF, end_offset, end_type
        pos += width
        previous = vid
    return pos, None, pos if end_offset is None else end_offset, end_type


def lap_field(size, payload):
    """Return (entry bit offset, value), or (insertion offset, None) if omitted."""
    return _lap_fields(size, payload)[:2]


def end_condition_type(size, payload):
    """Native EEndConditionType: 0 None, 1 NumLaps, 2 Time, 3..6 other modes."""
    return _lap_fields(size, payload)[3]


def _write_field(size, payload, offset, vid, width, value, present):
    if not present:
        if size + ID_BITS + width >= 1 << 14:
            raise ValueError("GameOptions too large to insert lap settings")
        low = payload & ((1 << offset) - 1)
        entry = vid | (value << ID_BITS)
        payload = low | (entry << offset) | ((payload >> offset) << (offset + ID_BITS + width))
        return size + ID_BITS + width, payload
    shift = offset + ID_BITS
    return size, (payload & ~(((1 << width) - 1) << shift)) | (value << shift)


def bounded_laps(size, payload, default=1, fixed=None, default_end=1):
    """Return (size, payload, lap count), explicitly writing a count in 1..10.

    Missing values retain the last accepted count (or one in a fresh lobby).
    Zero counts become one; the None end-condition becomes NumLaps. Timed and
    other explicit modes are preserved, including across sparse updates.
    An event already loading/racing supplies its fixed count.
    """
    offset, value, end_offset, end_type = _lap_fields(size, payload)
    selected = fixed if fixed is not None else default if value is None else value
    laps = min(MAX_LAPS, max(1, selected))
    condition = default_end if end_type is None else end_type
    if condition == 0 or fixed is not None:
        condition = 1  # None is an endless event, even with a positive NumLaps.
    if not 1 <= condition <= 6:
        raise ValueError("invalid GameOptions end condition")
    size, payload = _write_field(size, payload, offset, NUM_LAPS_ID, 32, laps, value is not None)
    size, payload = _write_field(size, payload, end_offset, END_CONDITION_ID, 3, condition, end_type is not None)
    return size, payload, laps


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
