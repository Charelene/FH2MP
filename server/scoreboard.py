"""Scoreboards (Rivals / leaderboards) for the LSP stand-in.

Stores the scores players submit (Scoreboard.SubmitScore / SubmitXpScore) and serves them back through
GetBestScore / GetRival / GetRowsAnthem. Boards are keyed (scoreboardType, scoreType, trackId); `score`
is a big-endian IEEE double carried as a u64 on the wire. Direction (lower is better for times, higher
for everything else) is a guess by scoreType, see ASCENDING. Real rows are topped up with deterministic
fake rivals (drawn from the Drivatar names) so a board is never empty; they are marked in the log only.
Persistence: server/data/scores.json.
"""
import hashlib
import json
import os
import struct
import time

ASCENDING = {1}          # scoreTypes where a smaller score wins (lap/race times) [guess]
XP_BOARD = (4, 9)        # (scoreboardType, scoreType) of the XP ladder [guess from SubmitXpScore traffic]
FAKE_ROWS = 40
SPEED_BOARDS = {9}       # scoreboardType of the speed trap / speed zone boards: score = m/s (32.565 shows as 117.23 km/h)


def dbl_to_u64(x):
    return struct.unpack(">Q", struct.pack(">d", x))[0]


def u64_to_dbl(u):
    return struct.unpack(">d", struct.pack(">Q", u))[0]


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


class ScoreStore:
    def __init__(self, data_dir, names):
        self.path = os.path.join(data_dir, "scores.json")
        self.names = list(dict.fromkeys(n for n in names if n)) or ["Rival"]   # unique, order kept
        self.subs = []     # submissions: dict
        self.xp = {}       # xuid(str) -> accumulated xp
        try:
            with open(self.path) as f:
                d = json.load(f)
            self.subs, self.xp = d.get("subs", []), d.get("xp", {})
        except FileNotFoundError:
            pass

    def _save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"subs": self.subs, "xp": self.xp}, f)
        os.replace(tmp, self.path)

    # ---- writes
    def submit(self, xuid, tag, score_type, d):
        s = {"xuid": xuid, "tag": tag, "sb": d["scoreboardType"], "st": score_type, "gsid": d["gameScoreboardId"],
             "track": d["trackId"], "car": d["carId"], "cls": d["carClassId"], "cpi": d["carPerformanceIndex"],
             "score": u64_to_dbl(d["score"]), "clean": d["isClean"], "stm": d["usedSTM"], "tcs": d["usedTCS"],
             "abs": d["usedABS"], "t": time.time()}
        self.subs.append(s)
        self.subs = self.subs[-5000:]
        self._save()
        return s

    def add_xp(self, xuid, xp):
        self.xp[str(xuid)] = self.xp.get(str(xuid), 0.0) + xp
        self._save()
        return self.xp[str(xuid)]

    # ---- reads
    def better(self, st, a, b):
        return a < b if st in ASCENDING else a > b

    def best_of(self, xuid, sb, st, track):
        best = None
        for s in self.subs:
            if s["xuid"] == xuid and s["sb"] == sb and s["st"] == st and (track in (-1, 65535) or s["track"] == track):
                if best is None or self.better(st, s["score"], best["score"]):
                    best = s
        if (sb, st) == XP_BOARD and str(xuid) in self.xp:
            x = self.xp[str(xuid)]
            if best is None or x > best["score"]:
                best = {"xuid": xuid, "tag": "", "sb": sb, "st": st, "track": track, "car": 0, "cls": 0, "cpi": 0.0,
                        "score": x, "clean": True, "stm": False, "tcs": False, "abs": False, "t": time.time()}
        return best

    def _fake(self, sb, st, track, anchor, n):
        """Deterministic fake rivals around `anchor` (the player's best, or a default scale)."""
        out = []
        for i in range(n):
            nm = self.names[(i * 7 + (hash_int(sb, st, track) % len(self.names))) % len(self.names)]
            r = hash_frac(sb, st, track, nm, i)
            if st in ASCENDING:
                base = anchor if anchor else 100.0
                score = base * (0.6 + 0.9 * r)                  # 60% .. 150% of the anchor time
            elif sb in SPEED_BOARDS:
                base = anchor if anchor else 50.0               # about 180 km/h
                score = max(20.0, base * (0.7 + 0.8 * r) if anchor else 35.0 + 45.0 * r)   # m/s
            else:
                base = anchor if anchor else 20000.0
                score = base * (0.05 + 3.0 * r * r)             # long tail of lower scores, a few above
            out.append({"xuid": 0x9100000000000 + (hash_int(nm) % 0xFFFFFF), "tag": nm[:15], "sb": sb, "st": st,
                        "track": track, "car": 100 + hash_int(nm, "car") % 500, "cls": hash_int(nm, "c") % 6,
                        "cpi": 0.3 + 0.6 * hash_frac(nm, "cpi"), "score": round(score, 3), "clean": r > 0.3,
                        "stm": False, "tcs": r < 0.5, "abs": True, "t": time.time() - 86400 * (1 + 20 * r)})
        return out

    def rows(self, xuid, sb, st, track, start_at, max_results):
        """Sorted best-per-player rows (real + fake) and the requesting player's row."""
        best = {}
        for s in self.subs:
            if s["sb"] == sb and s["st"] == st and (track in (-1, 65535) or s["track"] == track):
                cur = best.get(s["xuid"])
                if cur is None or self.better(st, s["score"], cur["score"]):
                    best[s["xuid"]] = s
        if (sb, st) == XP_BOARD:
            for k, v in self.xp.items():
                cur = best.get(int(k))
                if cur is None or v > cur["score"]:
                    best[int(k)] = {"xuid": int(k), "tag": "", "sb": sb, "st": st, "track": track, "car": 0, "cls": 0,
                                    "cpi": 0.0, "score": v, "clean": True, "stm": False, "tcs": False, "abs": False,
                                    "t": time.time()}
        me = best.get(xuid)
        real = list(best.values())
        fake = self._fake(sb, st, track, me["score"] if me else None, max(0, FAKE_ROWS - len(real)))
        allrows = real + fake
        allrows.sort(key=lambda s: s["score"], reverse=(st not in ASCENDING))
        for i, s in enumerate(allrows):
            s["position"] = i + 1
        mine = next((s for s in allrows if s["xuid"] == xuid), None)
        page = allrows[max(0, start_at - 1):max(0, start_at - 1) + max_results]
        return page, mine, len(real), len(fake)

    def rival_for(self, xuid, sb, st, track):
        rows, mine, _, _ = self.rows(xuid, sb, st, track, 1, 1000)
        if mine is None:
            return rows[len(rows) // 2] if rows else None
        above = [r for r in rows if r["position"] < mine["position"]]
        return above[-1] if above else None


def hash_int(*parts):
    return int.from_bytes(hashlib.sha1("|".join(str(p) for p in parts).encode()).digest()[:6], "big")


def hash_frac(*parts):
    return hash_int(*parts) / float(1 << 48)
