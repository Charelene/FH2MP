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


def audit_parts(words):
    """Problems in one car's part list: [(slot, word, text)]. A word is wrong when it is not -1 and not an id of its slot's table;
    the text says whether it exists in some other table."""
    out = []
    for k, w in enumerate(words):
        if w == NONE:
            continue
        table = P.SLOT_TABLE.get(k)
        if table is None:
            out.append((k, w, "this slot has no part type"))
        elif w not in P.TABLE_IDS[table]:
            out.append((k, w, f"not in {table}" + (" (an id of another part table)" if w in P.ALL_IDS else " (no such id)")))
    return out
