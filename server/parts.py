"""Reads the car's installed part ids (Parts[43], PartEnum order) out of the LobbyPlayer object a client creates (router 1) and checks them
against the game catalog (part_ids.py). Found 2026-10-20: the array sits at a bit offset that depends on the earlier variable-length fields
(910 in a Free Roam create, 919 in a Car Meet one), so it is located by its shape: 43 x 32-bit words where slots 3 and 26 (no part type)
are always -1 and every other word is -1 (none) or a catalog id."""
import part_ids as P

NONE = P.PART_NONE
LO = 0                          # earliest bit offset tried. Creates carry the array at 729-959; an update that changes the car (Road Trip car select,
                                # a car swap in Free Roam) carries it at bit 48 (dev captures 2026-10-22, 1462 and 1502 bits), which the old start (300)
                                # never reached: prod logged "no part list found" for those. The search runs up to the last place where 43 words fit.
MIN_BITS = 32 * P.PART_SLOTS    # an object shorter than the array itself cannot hold it


def find_parts(size, payload):
    """(bit offset, [43 words]) of the best match, or None. A match needs slots 3 and 26 == -1 and at least 34 of the other 41 words
    valid (so a few bad ids do not hide the array)."""
    best = None
    top = size - 32 * P.PART_SLOTS
    for o in range(LO, top + 1):
        if (payload >> (o + 32 * 3)) & 0xFFFFFFFF != NONE or (payload >> (o + 32 * 26)) & 0xFFFFFFFF != NONE:
            continue
        words = [(payload >> (o + 32 * k)) & 0xFFFFFFFF for k in range(P.PART_SLOTS)]
        good = sum(1 for k, w in enumerate(words) if k not in (3, 26) and (w == NONE or w in P.ALL_IDS))
        if good >= 34 and (best is None or good > best[0]):
            best = (good, o, words)
            if good == 41:
                break
    return None if best is None else (best[1], best[2])


def find_unknown_car(size, payload):
    """(bit offset, [43 words]) of a part list whose car is not in the catalog (a car pack the server does not have), or None. Such a list
    fails find_parts, because the car's own rows (body, bumpers, brakes, ...) are unknown too: here a word also counts when it is a row of
    the same car (id // 1000 == the body's car id; body rows are car id x 1000). The body must be such an unknown row."""
    best = None
    top = size - 32 * P.PART_SLOTS
    for o in range(LO, top + 1):
        if (payload >> (o + 32 * 3)) & 0xFFFFFFFF != NONE or (payload >> (o + 32 * 26)) & 0xFFFFFFFF != NONE:
            continue
        body = (payload >> (o + 64)) & 0xFFFFFFFF
        if body == NONE or body % 1000 or not 0 < body // 1000 < 100000 or body in P.TABLE_IDS["CarBody"]:
            continue
        words = [(payload >> (o + 32 * k)) & 0xFFFFFFFF for k in range(P.PART_SLOTS)]
        car = body // 1000
        good = sum(1 for k, w in enumerate(words) if k not in (3, 26) and (w == NONE or w in P.ALL_IDS or w // 1000 == car))
        if good >= 34 and (best is None or good > best[0]):
            best = (good, o, words)
    return None if best is None else (best[1], best[2])


def problem(slot, word):
    """Why `word` cannot be in `slot` (None when it can): the one classification audit_parts and neutralize share."""
    if word == NONE:
        return None
    table = P.SLOT_TABLE.get(slot)
    if table is None:
        return "this slot has no part type"
    if word in P.TABLE_IDS[table]:
        return None
    return f"not in {table}" + (" (an id of another part table)" if word in P.ALL_IDS else " (no such id)")


def must_replace(slot, word):
    """An id no client can build the car with: one that is in no part table at all, or anything but a camshaft row in the camshaft
    slot (slot 12: the engine's torque curve is read from that row, game+0x1064408). An id of another table elsewhere is only
    reported: an electric car's slot 0, for one, may hold its Motor row (car 2101), which is not an Engine row."""
    text = problem(slot, word)
    return text is not None and P.SLOT_TABLE.get(slot) is not None and slot != 2 and ("no such id" in text or slot == 12)


def audit_parts(words):
    """Problems in one car's part list: [(slot, word, text)]."""
    return [(k, w, t) for k, w in enumerate(words) if (t := problem(k, w)) is not None]


def _expected_owner(table, words):
    """Who a row in `table` must belong to for this car: its car id, or the engine / drivetrain its slot 0 / 1 row installs.
    None when it cannot be told (no valid car body, electric car, an engine row the game does not have)."""
    kind = P.OWNER_KIND.get(table)
    body = words[2]
    if kind == "car":
        return body // 1000 if body in P.TABLE_IDS["CarBody"] else None
    if kind == "engine":
        return P.INSTALLS["Engine"].get(words[0])
    if kind == "drivetrain":
        return P.INSTALLS["Drivetrain"].get(words[1])
    return None


def neutralize(words):
    """A copy of the part list that every client can build, and what was changed.

    An id the game data does not have makes the other clients crash when they build the car (prod 2026-10-21/22: 4 lobby wipes started
    in the same second a car with a made-up engine row appeared, e.g. 368335 on an Integra whose real engine rows are 368000-368002).
    Such a word (see must_replace) becomes the stock row of its owner, or -1 where the owner has no stock row (an optional part such as
    a turbo). Slots 0 and 1 are fixed first, because the engine and drivetrain sub-parts belong to the engine / drivetrain those rows
    install. Anything else that is odd (an id of another table, a row of another car or engine) is only reported in `odd`: nothing
    shows that it crashes anyone. Returns (new words, [(slot, old, new, why)], [(slot, word, text)])."""
    out, changes, odd = list(words), [], []
    if words[2] not in P.TABLE_IDS["CarBody"]:
        return out, changes, odd            # no car body row: the list may have been found at the wrong offset, never rewrite then
    for k in sorted(range(len(out)), key=lambda s: (s not in (0, 1, 2), s)):
        w = out[k]
        table = P.SLOT_TABLE.get(k)
        if must_replace(k, w):
            owner = _expected_owner(table, out)
            new = P.STOCK.get(table, {}).get(owner, NONE) if owner is not None else NONE
            out[k] = new
            changes.append((k, w, new, "no such id" if w not in P.ALL_IDS else "an id of another part table"))
            continue
        text = problem(k, w)
        if text is not None:
            odd.append((k, w, text))
            continue
        if w == NONE or table is None:
            continue
        owner = _expected_owner(table, out)
        have = P.ROW_OWNER.get(table, {}).get(w)
        if owner is not None and have is not None and have != owner:
            odd.append((k, w, f"{table} row of {P.OWNER_KIND[table]} {have}, this car's is {owner}"))
    return out, changes, odd


def stock_list(car):
    """The part list of `car` as it comes from the dealer: every slot holds its owner's stock row (-1 where the owner has none). None when
    the catalog has no body row for it. Used for the stand-in car (NUL_Car_00, the game's own placeholder) of an unknown car."""
    words = [NONE] * P.PART_SLOTS
    for k in (0, 1, 2):
        words[k] = P.STOCK.get(P.SLOT_TABLE[k], {}).get(car, NONE)
    if words[2] == NONE:
        return None
    for k, t in P.SLOT_TABLE.items():
        if k not in (0, 1, 2):
            owner = _expected_owner(t, words)
            words[k] = P.STOCK.get(t, {}).get(owner, NONE) if owner is not None else NONE
    return words


def extend(tables):
    """Adds the rows of another game database (part_tables.read_parts of the canonical database: car packs) to the catalog's. Rows the
    catalog has keep their owner and stock entries. -> number of part ids added."""
    added = 0
    for name, vals in tables["ids"].items():
        old = P.TABLE_IDS.get(name, frozenset())
        new = old.union(vals)
        added += len(new) - len(old)
        P.TABLE_IDS[name] = new
    P.ALL_IDS = frozenset().union(*P.TABLE_IDS.values())
    for name, k in tables["kind"].items():
        P.OWNER_KIND.setdefault(name, k)
    for src, dst in ((tables["owner"], P.ROW_OWNER), (tables["stock"], P.STOCK), (tables["installs"], P.INSTALLS)):
        for name, m in src.items():
            d = dst.setdefault(name, {})
            for key, v in m.items():
                d.setdefault(key, v)
    return added


CHOICE_VARS = (82, 83, 84, 85, 86, 87)          # PlayerChoices PerfIndex, Speed/Accel/Launch/Braking/Handling ratings: 8-bit id + 32-bit float each
CHOICE_CLASS_VAR = 81                            # CarClass: 8-bit id + 32-bit int, just before PerfIndex when the class changed


def find_choices(size, payload):
    """Where a LobbyPlayer object carries the car's class and performance index (Nerd::PlayerChoices vars 81/82, every var a 40-bit
    id + value pair, found on dev 2026-10-22: a car-select update is exactly [81 class][82 PI][83..87 ratings]; the class is left out
    when it did not change). Returns (bit offset of the PI value, bit offset of the class value or None) or None. The six ids 82..87 at a
    40-bit stride cannot appear in noise."""
    for o in range(0, size - 6 * 40 + 1):
        if all((payload >> (o + 40 * k)) & 0xFF == v for k, v in enumerate(CHOICE_VARS)):
            cls = o - 40 + 8 if o >= 40 and (payload >> (o - 40)) & 0xFF == CHOICE_CLASS_VAR else None
            return o + 8, cls
    return None


def find_short_choices(size, payload):
    """A short PlayerChoices update that is only [81 class][82 PI] (either may be missing), every var an 8-bit id + 32-bit value from bit 0:
    what a game sends right after the part list of a rental, with class R and PI 1.0 (dev 2026-10-25, Road Trip car select: the standings
    showed "R 999" for an S2 rental). -> find_choices' (PI offset, class offset or None), or None. The PI offset is None too when only the
    class is carried; read_short_choices / write_short_choices take that form."""
    if size not in (40, 80):
        return None
    ids = [(payload >> (40 * k)) & 0xFF for k in range(size // 40)]
    if ids not in ([CHOICE_CLASS_VAR, 82], [CHOICE_CLASS_VAR], [82]):
        return None
    cls_at = 8 if ids[0] == CHOICE_CLASS_VAR else None
    pi_at = 8 + 40 * ids.index(82) if 82 in ids else None
    return pi_at, cls_at


def read_short_choices(payload, found):
    """(class or None, PI or None) of a find_short_choices result."""
    import struct
    pi_at, cls_at = found
    pi = struct.unpack("<f", ((payload >> pi_at) & 0xFFFFFFFF).to_bytes(4, "little"))[0] if pi_at is not None else None
    cls = (payload >> cls_at) & 0xFFFFFFFF if cls_at is not None else None
    return cls, pi


def write_short_choices(payload, found, cls, pi):
    import struct
    pi_at, cls_at = found
    if pi_at is not None:
        payload = (payload & ~(0xFFFFFFFF << pi_at)) | (struct.unpack("<I", struct.pack("<f", pi))[0] << pi_at)
    if cls_at is not None:
        payload = (payload & ~(0xFFFFFFFF << cls_at)) | ((cls & 0xFFFFFFFF) << cls_at)
    return payload


def read_choices(payload, found):
    """(class or None, performance index) at what find_choices returned."""
    import struct
    pi_at, cls_at = found
    pi = struct.unpack("<f", ((payload >> pi_at) & 0xFFFFFFFF).to_bytes(4, "little"))[0]
    cls = (payload >> cls_at) & 0xFFFFFFFF if cls_at is not None else None
    if cls is not None and cls >= 1 << 31:
        cls -= 1 << 32
    return cls, pi


def write_choices(payload, found, cls, pi):
    """The payload with the class (when it is carried) and the performance index replaced."""
    import struct
    pi_at, cls_at = found
    payload = (payload & ~(0xFFFFFFFF << pi_at)) | (struct.unpack("<I", struct.pack("<f", pi))[0] << pi_at)
    if cls_at is not None:
        payload = (payload & ~(0xFFFFFFFF << cls_at)) | ((cls & 0xFFFFFFFF) << cls_at)
    return payload


PI_RANGE = (0.0, 1.5)          # stock cars 0.2-0.99, class R's cap is 1.1
RATING_RANGE = (0.0, 12.0)     # the six ratings are shown on a 0-10 scale (dev captures: 3-10)


def sanitize_choices(payload, found):
    """PerfIndex and the five ratings come from the client and every other client's grid and results screen shows them: a NaN or an absurd
    number is replaced by the nearest sane one (NaN by the middle of the range). -> (payload, [what was changed])."""
    import math
    import struct
    pi_at = found[0]
    notes = []
    for k, (name, lo, hi) in enumerate([("PerfIndex", *PI_RANGE)] + [("rating %d" % i, *RATING_RANGE) for i in range(1, 6)]):
        at = pi_at + 40 * k
        v = struct.unpack("<f", ((payload >> at) & 0xFFFFFFFF).to_bytes(4, "little"))[0]
        if math.isfinite(v) and lo <= v <= hi:
            continue
        fixed = (lo + hi) / 2 if not math.isfinite(v) else min(max(v, lo), hi)
        notes.append(f"{name} {v!r} -> {fixed:g}")
        payload = (payload & ~(0xFFFFFFFF << at)) | (struct.unpack("<I", struct.pack("<f", fixed))[0] << at)
    return payload, notes


def choices_update(cls, pi):
    """A server-written PlayerChoices update [81 class][82 PI] -> (bits, payload), relayed to the other clients."""
    import struct
    v = CHOICE_CLASS_VAR | ((cls & 0xFFFFFFFF) << 8) | (82 << 40) | (struct.unpack("<I", struct.pack("<f", pi))[0] << 48)
    return 80, v


def write_parts(payload, offset, words):
    """The object payload with the 43 words replaced at `offset`."""
    for k, w in enumerate(words):
        sh = offset + 32 * k
        payload = (payload & ~(0xFFFFFFFF << sh)) | ((w & 0xFFFFFFFF) << sh)
    return payload
