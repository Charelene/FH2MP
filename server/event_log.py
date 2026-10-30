"""A structured record of what happened to players, one JSON object per line (events.jsonl next to the game server's log).

The free-text server.log needs a day of grep to answer "how many sessions ended how, and why did events fail?" (prod 2026-10-29: 600 MB, no
dates on the lines). These lines carry the answer directly, and `summarize` is what tools/prod_report.py prints. Fields are short and carry
no addresses, tokens or names: an xuid is the only identifier. Writing never raises into the game server."""
import collections
import json
import threading
import time


class EventLog:
    def __init__(self, path):
        self.path = path
        self.file = None
        self.lock = threading.Lock()
        if path:
            try:
                self.file = open(path, "a", buffering=1)
            except OSError:
                self.file = None

    def emit(self, kind, **fields):
        if self.file is None:
            return
        line = json.dumps({"t": round(time.time(), 1), "kind": kind, **fields}, separators=(",", ":"), default=str)
        with self.lock:
            try:
                self.file.write(line + "\n")
            except (OSError, ValueError):
                pass


def read(lines, since=None):
    """Parsed events from JSON lines (a bad line is skipped), newer than `since` (epoch seconds)."""
    for raw in lines:
        try:
            e = json.loads(raw)
        except ValueError:
            continue
        if isinstance(e, dict) and isinstance(e.get("t"), (int, float)) and (since is None or e["t"] >= since):
            yield e


def summarize(events):
    """-> dict of counters for the report: sessions by end reason and length, events by outcome, refusals by reason, repairs by car."""
    out = collections.defaultdict(collections.Counter)
    players = set()
    for e in events:
        k = e.get("kind")
        if k == "session_end":
            out["sessions by world"][e.get("world_kind", "?")] += 1
            out["sessions by end"][f"{e.get('world_kind', '?')}: {e.get('why', '?')}"] += 1
            d = e.get("seconds")
            if isinstance(d, (int, float)):
                out["session length"]["<30 s" if d < 30 else "<2 min" if d < 120 else "<10 min" if d < 600 else "<1 h" if d < 3600 else ">=1 h"] += 1
            if e.get("xuid"):
                players.add(e["xuid"])
            if not e.get("entered"):
                out["sessions that never entered"][e.get("world_kind", "?")] += 1
        elif k == "event_end":
            outcome = ("finished" if e.get("finished") else "begun, nobody finished" if e.get("begun") else "never began")
            out["events by outcome"][f"{outcome}: {e.get('why', '?')}"] += 1
        elif k == "not_eligible":
            out["class refusals"][f"rule class {e.get('rule_class')} vs car class {e.get('class')}"
                                  + (" (rentable model)" if e.get("rentable") else "") + (" (repaired to stock)" if e.get("repaired") else "")] += 1
        elif k == "parts_repair":
            out["part repairs by car"][str(e.get("car"))] += 1
    out["players"]["distinct"] = len(players)
    return out
