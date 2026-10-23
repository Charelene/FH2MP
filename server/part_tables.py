"""Reads the part and car tables out of a game database (the catalog export, or a gamedbRC.slt such as the canonical one the launchers
sync, gamedb_sync.py). Shared by tools/make_part_ids.py, which writes part_ids.py once, and the game server, which adds the rows of the
canonical database (car packs) at run time. Only ids are read, plus media names and top speeds for the car policy."""
import sqlite3

# PartEnum -> table (names from UpgradeTypes, resolved through the game's strings: 12 = Camshaft, 13 = Valves, ...)
SLOT_TABLE = {
    0: "Engine", 1: "Drivetrain", 2: "CarBody", 4: "Brakes", 5: "SpringDamper", 6: "AntiSwayFront", 7: "AntiSwayRear", 8: "TireCompound",
    9: "RearWing", 10: "RimSizeFront", 11: "RimSizeRear", 12: "EngineCamshaft", 13: "EngineValves", 14: "EngineDisplacement",
    15: "EnginePistonsCompression", 16: "EngineFuelSystem", 17: "EngineIgnition", 18: "EngineExhaust", 19: "EngineIntake",
    20: "EngineFlywheel", 21: "EngineManifold", 22: "EngineRestrictorPlate", 23: "EngineOilCooling", 24: "EngineTurboSingle",
    25: "EngineTurboTwin", 27: "EngineCSC", 28: "EngineDSC", 29: "EngineIntercooler", 30: "DrivetrainClutch", 31: "DrivetrainTransmission",
    32: "DrivetrainDriveline", 33: "DrivetrainDifferential", 34: "CarBodyFrontBumper", 35: "CarBodyRearBumper", 36: "CarBodyHood",
    37: "CarBodySideSkirt", 38: "CarBodyTireWidthFront", 39: "CarBodyTireWidthRear", 40: "CarBodyWeight", 41: "CarBodyChassisStiffness",
    42: "MotorParts",
}

# owner column -> what it names; CarBody ids are car id x 1000, so a body-owned row is normalized to its car id
OWNER_COLUMNS = (("Ordinal", "car", 1), ("CarBodyID", "car", 1000), ("CarbodyId", "car", 1000), ("EngineID", "engine", 1),
                 ("DrivetrainID", "drivetrain", 1), ("DrivetrainId", "drivetrain", 1), ("MotorID", "motor", 1))


def open_ro(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


def read_parts(c):
    """-> dict(ids={table: sorted ids}, kind={table: owner kind}, owner={table: {id: owner}}, stock={table: {owner: stock id}},
    installs={"Engine"|"Drivetrain"|"Motor": {row: engine / drivetrain / motor id}})."""
    ids, kind, owner, stock = {}, {}, {}, {}
    for (t,) in c.execute("select name from sqlite_master where type='table' and name like 'List_Upgrade%'").fetchall():
        cols = [r[1] for r in c.execute(f'pragma table_info("{t}")')]
        key = "Id" if "Id" in cols else ("ID" if "ID" in cols else None)
        if not key:
            continue
        name = t[len("List_Upgrade"):]
        ids[name] = sorted(r[0] for r in c.execute(f'select "{key}" from "{t}"'))
        col = next((oc for oc in OWNER_COLUMNS if oc[0] in cols), None)
        if col is None:
            continue
        kind[name] = col[1]
        rows = c.execute(f'select "{key}", "{col[0]}", {"IsStock" if "IsStock" in cols else "Level = 0"} from "{t}"').fetchall()
        owner[name] = {i: o // col[2] for i, o, _ in rows}
        st = {}
        for i, o, s in sorted(rows):
            if s:
                st.setdefault(o // col[2], i)
        stock[name] = st
    installs = {t: {i: o for i, o in c.execute(f'select Id, "{col}" from "List_Upgrade{t}"')}
                for t, col in (("Engine", "EngineID"), ("Drivetrain", "DrivetrainID"), ("Motor", "MotorID"))}
    return {"ids": ids, "kind": kind, "owner": owner, "stock": stock, "installs": installs}


def read_cars(c):
    """-> {car id: (media name, stock top speed in km/h or None)} from Data_Car."""
    return {i: (name, round(mph * 1.609344) if mph else None) for i, name, mph in c.execute('select Id, MediaName, "TopSpeed-mph" from Data_Car')}


def read_car_choices(c):
    """-> {car id: (class id, performance index)} from Data_Car: what the PlayerChoices of a stock car say."""
    try:
        return {i: (cl, round(pi, 6)) for i, cl, pi in c.execute("select Id, ClassID, PerformanceIndex from Data_Car")}
    except sqlite3.Error:
        return {}


def read_rentals(c):
    """-> set of car ids in RentalCars (empty when the table is missing)."""
    try:
        return {r[0] for r in c.execute("select CarId from RentalCars")}
    except sqlite3.Error:
        return set()
