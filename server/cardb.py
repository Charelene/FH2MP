"""Read-only lookup of car stats from the user's own game database (media/stripped/gamedbRC.slt).

Nothing from the database is stored in this repo; it is opened in place, read-only, on demand.
"""
import math
import os
import re
import sqlite3


class CarDb:
    def __init__(self, exe_path, catalog=None):
        """`catalog`: path of the derived catalog (server/catalog.py), a copy of the game database that also holds resolved
        strings, route metrics and views. Without it the game's own database is opened in place, read-only."""
        self.mount = os.path.dirname(os.path.abspath(exe_path))
        self.path = os.path.join(self.mount, "media", "stripped", "gamedbRC.slt")
        self.catalog = catalog if catalog and os.path.exists(catalog) else None
        self.db = None
        src = self.catalog or (self.path if os.path.exists(self.path) else None)
        if src:
            self.db = sqlite3.connect(f"file:{src}?mode=ro" + ("" if self.catalog else "&immutable=1"), uri=True, check_same_thread=False)
            self.db.row_factory = sqlite3.Row

    def row(self, car_id):
        if self.db is None:
            return None
        return self.db.execute("select * from Data_Car where Id=?", (car_id,)).fetchone()

    def car_struct(self, car_id):
        """Values for the `car` struct of Gifting.RetrieveCarGift_Results (None if the car is unknown)."""
        r = self.row(car_id)
        if r is None:
            return None
        pi = float(r["PerformanceIndex"] or 0)
        return {
            "make": r["MakeID"], "carId": r["Id"], "garageId": 0, "manColorIndex": 0,
            "countryId": r["CountryID"] or 0, "displacementId": int(r["Displacement"] or 0),
            "aspirationTypeId": r["AspirationTypeId"] or 0, "performanceIndex": pi,
            "performanceIndexFriendly": int(round(pi * 1000)), "carClass": r["ClassID"], "year": r["Year"],
            "peakPower": float(r["SimPeakPower"] or 0), "peakTorque": float(r["SimPeakTorque"] or 0),
            "curbWeight": float(r["CurbWeight"] or 0), "weightDistribution": float(r["WeightDistribution"] or 0),
            "powertrainId": r["PowertrainID"] or 0, "isLiveryLocked": False, "isUnicorn": bool(r["IsUnicorn"]),
            "isReward": False, "wasOriginallyFromAuctionHouse": False, "wasOriginallyGifted": True,
            "speedRating": float(r["SpeedRating"] or 0), "accelerationRating": float(r["AccelerationRating"] or 0),
            "launchRating": float(r["LaunchRating"] or 0), "brakingRating": float(r["BrakingRating"] or 0),
            "handlingRating": float(r["HandlingRating"] or 0), "versionedLiveryId": 0, "versionedTuneId": 0,
        }

    def make_model(self, car_id):
        r = self.row(car_id)
        return (r["MakeID"], r["ModelId"]) if r is not None else None

    def fake_pool(self):
        """(carId, classId, performanceIndex, topSpeed m/s) of every car, for synthesized leaderboard rows."""
        if self.db is None:
            return []
        q = 'select Id, ClassID, PerformanceIndex, "TopSpeed-mph" as top from Data_Car'
        q += r" where MediaName not like 'PG\_%' escape '\' and MediaName not like '%TF\_%' escape '\'"   # traffic vehicles, hidden TF variants
        return [(r["Id"], r["ClassID"], float(r["PerformanceIndex"] or 0), float(r["top"] or 0) * 0.44704)
                for r in self.db.execute(q)]

    def route_info(self, track_id):
        """(length in metres, start-to-finish distance in metres) of a track's race route, or None. Read in place from
        media/tracks/anthem/trackroutes/route<Tracks.Route>.nt (XML locators): the ordered `route_waypoint_NNN` plus the
        `finish_line` give the length (the chord polyline, a little shorter than the road [inferred]); `start_location_000`
        to `finish_line` tells a closed circuit (about 10-35 m) from a point-to-point route (km) [confirmed on 411 vs 8001]."""
        cache = self.__dict__.setdefault("_route_info", {})
        if track_id in cache:
            return cache[track_id]
        info = None
        if self.catalog:
            r = self.db.execute("SELECT length_m, start_finish_m FROM routes WHERE track_id=?", (track_id,)).fetchone()
            cache[track_id] = (r[0], r[1]) if r else None
            return cache[track_id]
        try:
            r = self.db.execute("select Route from Tracks where id=?", (track_id,)).fetchone() if self.db else None
            if r is not None and r[0]:
                path = os.path.join(self.mount, "media", "tracks", "anthem", "trackroutes",
                                    f"route{int(r[0])}.nt")
                with open(path, "rb") as f:
                    xml = f.read().decode("latin1")
                locs = {n: (float(x), float(y), float(z)) for n, x, y, z in re.findall(
                    r"<Name value='([^']*)'/>.*?value\._41=\"([-0-9.e+]+)\" value\._42=\"([-0-9.e+]+)\" "
                    r"value\._43=\"([-0-9.e+]+)\"", xml, re.S)}
                wp = [locs[n] for n in sorted(locs) if re.fullmatch(r"route_waypoint_\d+", n)]
                fin, st = locs.get("finish_line"), locs.get("start_location_000")
                pts = wp + ([fin] if fin else [])
                if len(pts) >= 2 and fin and st:
                    length = sum(math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1))
                    if length > 100:
                        info = (length, math.dist(st, fin))
        except (OSError, ValueError, sqlite3.Error):
            info = None
        cache[track_id] = info
        return info

    def route_length(self, track_id):
        i = self.route_info(track_id)
        return i[0] if i else None

    def is_circuit(self, track_id):
        i = self.route_info(track_id)
        return bool(i and i[1] < 150)
