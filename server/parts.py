"""Reads the car's installed part ids (Parts[43], PartEnum order) out of the LobbyPlayer object a client creates (router 1) and checks them
against the game catalog (part_ids.py). Found 2026-10-20: the array sits at a bit offset that depends on the earlier variable-length fields
(910 in a Free Roam create, 919 in a Car Meet one), so it is located by its shape: 43 x 32-bit words where slots 3 and 26 (no part type)
are always -1 and every other word is -1 (none) or a catalog id."""
import part_ids as P

NONE = P.PART_NONE
LO = 300                        # earliest bit offset tried; the array has been seen at 910 and 919, and objects of 3129 bits exist (Road Trip), so the
                                # search runs up to the last place where 43 words still fit


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


def write_parts(payload, offset, words):
    """The object payload with the 43 words replaced at `offset`."""
    for k, w in enumerate(words):
        sh = offset + 32 * k
        payload = (payload & ~(0xFFFFFFFF << sh)) | ((w & 0xFFFFFFFF) << sh)
    return payload
