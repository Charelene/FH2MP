"""Judges a Rivals run from its ghost (ghost.py). Returns a verdict, its reasons and the measurements, and touches nothing itself.

HARD means the run cannot be real, so it is hidden automatically; FLAG only puts it in the review queue. Rules were measured on all 17,011
stored ghosts (2026-10-30) against runs the maintainer checked in game. Highest value of any legit player, then the threshold:
p99 speed 112.9/127.4 m/s, p99 acceleration 16.2/30 m/s2, p99 climb 14.8/30 m/s, specific power 733/1000 W/kg. Speed from positions
agrees with the car's stored velocity within 0.4% (p1-p99), so these are real figures. The ghost is written by the client: this catches
tools that change the physics, not a forged ghost, and subtle cheats stay below every threshold. Replays remain the final word.
"""
import math
import struct

import ghost

VERSION = 1
PASS, FLAG, HARD = "pass", "flag", "hard"

END_TOLERANCE = 0.5        # s, run time against the last record: 3 of 17,011 differ by more than 0.1 s
HEADER_TOLERANCE = 0.05    # s, run time against the ghost's header: all 17,011 are within it
MAX_FRAME_GAP = 2.0        # s, the game records about 5 Hz; no real ghost has a gap of 1 s
MIN_FRAMES = 8
HIGH_SPEED = 40.0          # m/s, specific power is read above this, where grip no longer limits a car
MIN_HIGH_SAMPLES = 10
MAX_SPEED = 127.4          # m/s, as scoreboard.SPEED_CAP: fastest a car really goes
TELEPORT_SPEED = 160.0     # m/s between two frames; one is a track reset, a run of them is not
HARD_LIMITS = {"speed": MAX_SPEED, "acceleration": 30.0, "climb": 30.0, "power": 1000.0}
FLAG_LIMITS = {"speed": 120.0, "acceleration": 20.0, "power": 700.0}
MAX_TELEPORTS = 5


def _pct(v, p):
    return v[min(len(v) - 1, int(len(v) * p))]


def _steps(frames):
    """(mid time, vx, vy, vz, horizontal speed) between frames, in m/s."""
    out = []
    for a, b in zip(frames, frames[1:]):
        dt = b.t - a.t
        if 0.02 < dt <= 1:
            vx, vy, vz = (b.x - a.x) / dt, (b.y - a.y) / dt, (b.z - a.z) / dt
            out.append(((a.t + b.t) / 2, vx, vy, vz, math.hypot(vx, vz)))
    return out


def measure(g):
    """The numbers the rules read, or None if the ghost holds too little driving to judge."""
    f = g.frames
    steps = _steps(f)
    speeds, accels, powers, high = [], [], [], []
    for a, b in zip(steps, steps[1:]):
        dt = b[0] - a[0]
        v = (a[4] + b[4]) / 2
        if dt <= 0.02 or dt > 1 or v < 5 or a[4] > TELEPORT_SPEED or b[4] > TELEPORT_SPEED:
            continue
        along = ((b[1] - a[1]) * (a[1] + b[1]) + (b[3] - a[3]) * (a[3] + b[3])) / (2 * v * dt)
        speeds.append(v)
        accels.append(along)
        powers.append(along * v)
        if v > HIGH_SPEED:
            high.append(along * v)
    if len(speeds) < MIN_FRAMES:
        return None
    speeds.sort()
    accels.sort()
    high.sort()
    climbs = sorted(s[2] for s in steps)
    return {"frames": len(f), "first": f[0].t, "gap": max(b.t - a.t for a, b in zip(f, f[1:])),
            "speed": _pct(speeds, .99), "acceleration": _pct(accels, .99), "climb": _pct(climbs, .99),
            "power": _pct(high, .95) if len(high) >= MIN_HIGH_SAMPLES else 0.0, "high_samples": len(high),
            "teleports": sum(1 for s in steps if s[4] > TELEPORT_SPEED)}


def header_time(header, score):
    """True if the ghost's header carries `score`. The field moves with the strings before it (offsets 2082-2095 and beyond)."""
    return any(abs(struct.unpack_from("<d", header, o)[0] - score) < HEADER_TOLERANCE for o in range(2040, min(2200, len(header) - 8)))


def header_car(header, car):
    return any(struct.unpack_from("<I", header, o)[0] == car for o in range(2900, min(3500, len(header) - 4)))


def check(blob, score, car):
    """(verdict, reasons, measurements) of a stored ghost for the submitted time and car."""
    try:
        g = ghost.parse(blob)
    except ghost.GhostError as e:
        return HARD, [f"unreadable ghost: {e}"], {}
    hard, flags = [], []
    if abs(g.last_t - score) > END_TOLERANCE:
        hard.append(f"ghost ends at {g.last_t:.3f} s, the score says {score:.3f} s")
    if not header_time(g.header, score):
        hard.append("the score is not in the ghost's header")
    if not header_car(g.header, car):
        flags.append("the car is not in the ghost's header")       # also true of a real run of the maintainer's: not proof
    m = measure(g)
    if m is None:
        hard.append("too little driving in the ghost")
        return HARD, hard, {}
    m["last"] = g.last_t
    if m["gap"] > MAX_FRAME_GAP:
        hard.append(f"{m['gap']:.1f} s missing between frames")
    if m["teleports"] >= MAX_TELEPORTS:
        hard.append(f"{m['teleports']} jumps across the map")
    for rule, limit in HARD_LIMITS.items():
        if m[rule] > limit:
            hard.append(f"{rule} {m[rule]:.1f} is above {limit:g}")
        elif rule in FLAG_LIMITS and m[rule] > FLAG_LIMITS[rule]:
            flags.append(f"{rule} {m[rule]:.1f} is above {FLAG_LIMITS[rule]:g}")
    if hard:
        return HARD, hard + flags, m
    return (FLAG if flags else PASS), flags, m
