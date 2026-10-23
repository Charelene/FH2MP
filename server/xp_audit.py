"""XP integrity ("the IRS"): which SubmitXpScore awards count, and an audit of the awards already in the log.

SubmitXpScore carries only the amount (4-byte body), so where an award came from has to be inferred. Facts (prod lsp.log 2026-10-18..21,
87,098 awards): p50 50, p99 7,500, p99.9 15,000, largest believable 34,312. The longest route is 46.0 km (track 1003, route 4999) at
100 XP/km (economy_settings in fh2mp_lsp.py), so no single award can honestly come near MAX_AWARD. 7,500 and 15,000 are common to 135
and 90 accounts (real payouts); the cheated awards are flat amounts (12,500,000, 10,000,000, 130,000,000, 12,500 x 168) on few accounts.

Rules (user decisions 2026-10-21):
  * an award above MAX_AWARD (env FH2MP_XP_MAX, default 56000 * XP_SCALE), not finite or not positive is rejected: stored, never counted;
  * anything else counts, and `suspects` finds the amounts worth a closer look: one that few accounts ever get (fewer than RARE_USERS)
    but one account repeats at least REPEATS times. Suspects are reported, never removed automatically.

CLI: python3 -I server/xp_audit.py report|apply /var/log/fh2mp/lsp.log [--year 2026 --start-date 10-18] [--yes]
     (report is read-only; apply needs FH2MP_PG_DSN and --start-date, is a dry run without --yes, and opening the database
     applies db.MIGRATIONS: run it only once the LSP with migration 7 is deployed)
"""
import collections
import datetime
import math
import os
import re
import sys

# Online race XP per km is multiplied by XP_SCALE (env FH2MP_XP_SCALE, default 10; maintainer 2026-10-23: race XP only, online only;
# fh2mp_lsp.economy_settings), and so is the largest honest award.
XP_SCALE = float(os.environ.get("FH2MP_XP_SCALE", "10"))
MAX_AWARD = float(os.environ.get("FH2MP_XP_MAX", str(56000 * XP_SCALE)))
RARE_USERS = 10          # an amount fewer accounts than this ever received is "rare"
REPEATS = 20             # ... and one account receiving it this often is a suspect
BEFORE_PLAUSIBLE = 5e6   # a balance carried into the log above this is reported (the honest leader had under 1M on 2026-10-20, issue 18)

OK, REJECTED = "ok", "rejected"


def judge(xp):
    """(verdict, reason) for one award."""
    if not isinstance(xp, (int, float)) or not math.isfinite(xp):
        return REJECTED, "not a finite number"
    if xp < 0:
        return REJECTED, "negative"
    if xp > MAX_AWARD:
        return REJECTED, f"above the largest possible award ({MAX_AWARD:g})"
    return OK, ""


def suspects(awards, rare_users=RARE_USERS, repeats=REPEATS):
    """awards: iterable of (xuid, xp) for counted awards. Returns {xuid: [(amount, times, accounts with that amount)]}."""
    by_amount = collections.defaultdict(collections.Counter)
    for xuid, xp in awards:
        by_amount[xp][xuid] += 1
    out = collections.defaultdict(list)
    for amount, users in by_amount.items():
        if len(users) >= rare_users:
            continue
        for xuid, n in users.items():
            if n >= repeats:
                out[xuid].append((amount, n, len(users)))
    return dict(out)


# ---- the existing log (lsp.log): `#N POST ... SubmitXpScore body=4 user=0x..` then `HH:MM:SS   SubmitXpScore +X -> total T` (since
# 2026-10-22 the award line ends with ` user=0x..` itself)
_POST = re.compile(r"SubmitXpScore body=\d+ user=(0x[0-9a-f]+)")
_AWARD = re.compile(r"^(\d\d):(\d\d):(\d\d)   SubmitXpScore \+(\S+) -> total (\S+)(?: user=(0x[0-9a-f]+))?")


def _close(a, b):
    """Totals are printed with 6 significant digits ({:g})."""
    return abs(a - b) <= max(1.0, 1e-5 * abs(b))


def parse_log(lines, start=None):
    """[(xuid or None, xp, total_after, unix_time or None)] in log order. The LSP serves requests on threads, so the request line and
    its award line of two submissions can interleave: an award goes to the waiting request whose account total plus the award gives
    the printed total, else to the only waiting request (or the only one whose account has no total yet); otherwise it cannot be told
    and the xuid is None (audit skips it, report counts it). Log lines carry no date: `start` (a datetime.date, the server's local
    time like the log) is the day of the first line, and a clock that goes backwards by more than an hour means the next day."""
    out, pending, last_total, day, last = [], [], {}, start, None
    for line in lines:
        m = _POST.search(line)
        if m:
            pending.append(int(m.group(1), 16))
            del pending[:-16]                       # a request that never logged its award must not stay a candidate forever
            continue
        m = _AWARD.match(line)
        if not m:
            continue
        hh, mm, ss = int(m.group(1)), int(m.group(2)), int(m.group(3))
        xp, total = float(m.group(4)), float(m.group(5))
        sec = hh * 3600 + mm * 60 + ss
        if last is not None and sec < last - 3600:
            day = day + datetime.timedelta(days=1) if day else None
        last = sec
        if m.group(6):
            user = int(m.group(6), 16)
        else:
            fits = sorted((abs(last_total[u] + xp - total), u) for u in set(pending) if u in last_total and _close(last_total[u] + xp, total))
            fresh = [u for u in set(pending) if u not in last_total]
            if fits:                                # the closest running total; a tie after rounding cannot be told apart
                user = fits[0][1] if len(fits) == 1 or fits[0][0] < fits[1][0] else None
            else:
                user = pending[0] if len(set(pending)) == 1 else (fresh[0] if len(fresh) == 1 else None)
        if user in pending:
            pending.remove(user)
        if user is not None:
            last_total[user] = total
        t = datetime.datetime(day.year, day.month, day.day, hh, mm, ss).timestamp() if day is not None else None
        out.append((user, xp, total, t))
    return out


def audit(awards):
    """Per account from parse_log output: {"before", "counted", "legit", "rejected", "rejected_n", "last_total", "suspects",
    "removals": [(t, xp)]}. `before` is the balance the account already had when the log starts (first total minus first award: the
    log prints totals with 6 significant digits, so the legit total is built from the exact awards, not from the printed totals)."""
    acc = collections.defaultdict(lambda: {"before": None, "counted": 0.0, "rejected": 0.0, "rejected_n": 0, "last_total": 0.0,
                                           "removals": []})
    counted, unknown, unknown_rejected = [], 0, 0
    for xuid, xp, total, t in awards:
        if xuid is None:
            unknown += 1
            unknown_rejected += judge(xp)[0] != OK
            continue
        a = acc[xuid]
        if a["before"] is None:
            a["before"] = max(0.0, total - xp) if math.isfinite(xp) else total
        a["last_total"] = total
        verdict, _ = judge(xp)
        if verdict == OK:
            a["counted"] += xp
            counted.append((xuid, xp))
        else:
            a["rejected"] += xp if math.isfinite(xp) else 0.0
            a["rejected_n"] += 1
            a["removals"].append((t, xp))
    sus = suspects(counted)
    for xuid, a in acc.items():
        a["suspects"] = sus.get(xuid, [])
        a["legit"] = a["before"] + a["counted"]
    acc = dict(acc)
    if unknown:
        acc[None] = {"unattributed": unknown, "unattributed_rejected": unknown_rejected}
    return acc


def report(acc):
    unknown = acc.get(None, {}).get("unattributed", 0)
    unknown_bad = acc.get(None, {}).get("unattributed_rejected", 0)
    acc = {x: a for x, a in acc.items() if x is not None}
    rows = [(x, a) for x, a in acc.items() if a["rejected_n"] or a["suspects"] or a["before"] > BEFORE_PLAUSIBLE]
    rows.sort(key=lambda r: -(r[1]["rejected"] + sum(s[0] * s[1] for s in r[1]["suspects"])))
    lines = [f"{len(acc)} accounts with awards, {sum(1 for _, a in rows if a['rejected_n'])} with rejected awards, "
             f"{sum(1 for _, a in rows if a['suspects'])} with suspect amounts"
             + (f", {unknown} award(s) that could not be attributed (interleaved requests; {unknown_bad} of them impossible)" if unknown else "")]
    for x, a in rows:
        s = "; ".join(f"{amt:g} x{n} (only {u} account(s) ever got it)" for amt, n, u in a["suspects"])
        lines.append(f"{x:#x}: total now {a['last_total']:.0f}, rejected {a['rejected_n']} award(s) = {a['rejected']:.0f}, "
                     f"legit total {a['legit']:.0f} (of which {a['before']:.0f} from before the log)"
                     + (" BEFORE-LOG BALANCE UNEXPLAINED" if a["before"] > BEFORE_PLAUSIBLE else "") + (f"; suspect: {s}" if s else ""))
    return "\n".join(lines)


def apply(dbh, acc, write=False):
    """Takes the rejected awards found in the log off the stored totals: new = stored - rejected, but never below the legit total from the
    log (XP earned after the log was read is kept) and never above the stored total (one an operator already lowered stays). Club XP is the gain over club_members.xp_base: a removed award from before the player
    joined the club was in the base, so the base drops by that part. Each removed award goes into xp_awards (verdict rejected, reason
    `backfill`) so the change can be traced and reversed. Returns one line per account; writes only when `write`."""
    out = []
    todo = [(x, a) for x, a in acc.items() if x is not None and a["rejected_n"]]
    with dbh.tx() as c:
        for xuid, a in todo:
            r = c.execute("SELECT xp FROM player_xp WHERE xuid=?", (xuid,)).fetchone()
            if r is None:
                continue
            cur = r[0]
            new = min(cur, max(a["legit"], cur - a["rejected"]))
            m = c.execute("SELECT joined, xp_base FROM club_members WHERE xuid=?", (xuid,)).fetchone()
            base_cut = 0.0
            if m is not None:
                base_cut = min(m["xp_base"], sum(xp for t, xp in a["removals"] if t is not None and t < m["joined"]))
            out.append(f"{xuid:#x}: {cur:.0f} -> {new:.0f}" + (f", club base -{base_cut:.0f}" if base_cut else ""))
            if not write:
                continue
            for t, xp in a["removals"]:
                c.execute("INSERT INTO xp_awards (xuid, xp, at, verdict, reason) VALUES (?,?,?,?,?)",
                          (xuid, xp if math.isfinite(xp) else 0.0, t or 0.0, REJECTED, "backfill: " + judge(xp)[1]))
            c.execute("UPDATE player_xp SET xp=?, updated=? WHERE xuid=?", (new, datetime.datetime.now().timestamp(), xuid))
            if base_cut:
                c.execute("UPDATE club_members SET xp_base = xp_base - ? WHERE xuid=?", (base_cut, xuid))
    return out


def main(argv):
    if len(argv) < 2 or argv[0] not in ("report", "apply"):
        print("\n".join(__doc__.strip().splitlines()[-2:]))
        return 2
    start = None
    if "--start-date" in argv:
        y = int(argv[argv.index("--year") + 1]) if "--year" in argv else datetime.date.today().year
        mo, d = (int(v) for v in argv[argv.index("--start-date") + 1].split("-"))
        start = datetime.date(y, mo, d)
    with open(argv[1], errors="replace") as f:
        acc = audit(parse_log(f, start))
    if argv[0] == "report":
        print(report(acc))
        return 0
    if start is None:
        print("apply needs --start-date (the day of the log's first line): club bases are corrected by when each award happened")
        return 2
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))    # -I leaves the script's folder off the path
    import db
    dbh = db.open_db()
    write = "--yes" in argv
    print("\n".join(apply(dbh, acc, write)) or "nothing to change")
    print("written" if write else "dry run: add --yes to write (back up player_xp and club_members first)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
