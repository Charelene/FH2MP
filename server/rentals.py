"""Rental cars of the game's database (`RentalCars`, one column: CarId).

The game lists a rental car for an event when it is in RentalCars and passes the event's restrictions (class, era, make, body ... the same
filter as the garage: "SELECT Id FROM Data_Car AS Garage JOIN RentalCars ON ... WHERE <restrictions>", FUN_1408d1300). The stock table has
22 cars, 3-4 per class, picked for the retail events: of the 59 themes Monthly Rivals can use, none but a few country themes has even one
fitting rental, so e.g. "Retro Icons Cup, C class" offered nothing (2026-10-22). `plan_extra` picks the fewest cars to add to the table so
that every theme has min(target, cars in the theme) rentals; tools/make_rentals_db.py writes them into the canonical gamedbRC.slt that the
launchers sync (gamedb_sync.py). Rivals only generate events a rental covers (rivals.RivalCatalog.rentable).
"""

RENTAL_TARGET = 2          # rentals every event is guaranteed (2-3 to choose from; 3 would need 77 added cars instead of 46)
RENTAL_MIN = 2             # an event is only generated when at least this many (or all, when its theme has fewer cars) fit


def plan_extra(theme_cars, rentals, candidates, target=RENTAL_TARGET):
    """Greedy cover. theme_cars: {theme key: set of car ids the theme allows}; rentals: car ids already rental; candidates: car ids that
    may be added (not traffic-only models ...). Returns (cars to add in the order picked, {theme key: cars still missing}).
    A theme needs min(target, len(its cars)) rentals; each pick is the candidate that closes the most missing slots (ties: lowest id)."""
    rentals = set(rentals)
    need = {k: max(0, min(target, len(cars)) - len(cars & rentals)) for k, cars in theme_cars.items()}
    by_car = {}
    for k, cars in theme_cars.items():
        if need[k]:
            for c in cars:
                by_car.setdefault(c, []).append(k)
    picks = []
    pool = set(candidates) - rentals
    while True:
        best, gain = None, 0
        for c in sorted(pool):
            g = sum(1 for k in by_car.get(c, ()) if need[k])
            if g > gain:
                best, gain = c, g
        if best is None:
            break
        picks.append(best)
        pool.discard(best)
        for k in by_car.get(best, ()):
            if need[k]:
                need[k] -= 1
    return picks, {k: n for k, n in need.items() if n}


def covered(cars, rentals):
    """How many rentals an event with this set of allowed cars has."""
    return len(set(cars) & set(rentals))


def enough(cars, rentals, minimum=RENTAL_MIN):
    """The event offers enough rentals: `minimum`, or every car it allows when it allows fewer."""
    cars = set(cars)
    return covered(cars, rentals) >= min(minimum, len(cars))
