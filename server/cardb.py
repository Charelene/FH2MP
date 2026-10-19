"""Read-only lookup of car, track and route data in the game catalog (PostgreSQL schema `game`, loaded by tools/load_gamedata_pg.py
from the bundle tools/export_gamedata.py builds). The server never reads the game's own files.
"""


class CarDb:
    def __init__(self, database):
        """`database`: the db.Database whose `game` schema holds the catalog (tools/load_gamedata_pg.py): a copy of the game's tables
        plus resolved strings, route metrics and views. No game file is touched."""
        self.db = database

    def row(self, car_id):
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
        q = 'select Id, ClassID, PerformanceIndex, "topspeed-mph" as top from Data_Car'
        q += r" where MediaName not like 'PG\_%' escape '\' and MediaName not like '%TF\_%' escape '\'"   # traffic vehicles, hidden TF variants
        return [(r["Id"], r["ClassID"], float(r["PerformanceIndex"] or 0), float(r["top"] or 0) * 0.44704)
                for r in self.db.execute(q)]

    def route_info(self, track_id):
        """(length in metres, start-to-finish distance in metres) of a track's race route, or None. Read from the catalog table
        `routes`, which the exporter fills from media/tracks/anthem/trackroutes/route<Tracks.Route>.nt (XML locators): the ordered
        `route_waypoint_NNN` plus the `finish_line` give the length (the chord polyline, a little shorter than the road [inferred]);
        `start_location_000` to `finish_line` tells a closed circuit (about 10-35 m) from a point-to-point route (km) [confirmed on
        411 vs 8001]."""
        cache = self.__dict__.setdefault("_route_info", {})
        if track_id not in cache:
            r = self.db.execute("SELECT length_m, start_finish_m FROM routes WHERE track_id=?", (track_id,)).fetchone()
            cache[track_id] = (r[0], r[1]) if r else None
        return cache[track_id]

    def route_length(self, track_id):
        i = self.route_info(track_id)
        return i[0] if i else None

    def is_circuit(self, track_id):
        i = self.route_info(track_id)
        return bool(i and i[1] < 150)
