"""Read-only lookup of car stats from the user's own game database (media/stripped/gamedbRC.slt).

Nothing from the database is stored in this repo; it is opened in place, read-only, on demand.
"""
import os
import sqlite3


class CarDb:
    def __init__(self, exe_path):
        self.path = os.path.join(os.path.dirname(os.path.abspath(exe_path)), "media", "stripped", "gamedbRC.slt")
        self.db = None
        if os.path.exists(self.path):
            self.db = sqlite3.connect(f"file:{self.path}?mode=ro&immutable=1", uri=True, check_same_thread=False)
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
