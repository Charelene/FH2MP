// Package cardb is the read-only lookup of car, track and route data in the game catalog (schema `game`) (= server/cardb.py).
package cardb

import (
	"math"
	"sync"

	"fh2mp/internal/db"
	"fh2mp/internal/xls"
)

// CarDb reads the catalog.
type CarDb struct {
	DB    *db.DB
	mu    sync.Mutex
	route map[int64]*[2]float64
	known map[int64]bool
}

// New makes a CarDb on the database whose `game` schema holds the catalog.
func New(d *db.DB) *CarDb { return &CarDb{DB: d, route: map[int64]*[2]float64{}} }

// KnownIDs is the set of every car id of the catalog (official DLC included; car packs that are not in the server's database are not). Read once;
// an error is returned and not cached.
func (c *CarDb) KnownIDs() (map[int64]bool, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.known != nil {
		return c.known, nil
	}
	rows, err := c.DB.Query("select Id from Data_Car")
	if err != nil {
		return nil, err
	}
	ids := make(map[int64]bool, len(rows))
	for i := range rows {
		ids[rows[i].IntAt(0)] = true
	}
	c.known = ids
	return ids, nil
}

// Row returns the Data_Car row (nil = unknown car).
func (c *CarDb) Row(carID int64) (*db.Row, error) {
	return c.DB.One("select * from Data_Car where Id=?", carID)
}

func nz(r *db.Row, col string) float64 {
	if r.IsNull(col) {
		return 0
	}
	return r.Float(col)
}

// CarStruct is the values of the `car` struct of Gifting.RetrieveCarGift_Results (nil if the car is unknown).
func (c *CarDb) CarStruct(carID int64) (*xls.Struct, error) {
	r, err := c.Row(carID)
	if err != nil || r == nil {
		return nil, err
	}
	pi := nz(r, "PerformanceIndex")
	s := xls.NewStruct()
	s.Set("make", r.Int("MakeID"))
	s.Set("carId", r.Int("Id"))
	s.Set("garageId", int64(0))
	s.Set("manColorIndex", int64(0))
	s.Set("countryId", int64(nz(r, "CountryID")))
	s.Set("displacementId", int64(nz(r, "Displacement")))
	s.Set("aspirationTypeId", int64(nz(r, "AspirationTypeId")))
	s.Set("performanceIndex", pi)
	s.Set("performanceIndexFriendly", int64(math.RoundToEven(pi*1000)))
	s.Set("carClass", r.Int("ClassID"))
	s.Set("year", r.Int("Year"))
	s.Set("peakPower", nz(r, "SimPeakPower"))
	s.Set("peakTorque", nz(r, "SimPeakTorque"))
	s.Set("curbWeight", nz(r, "CurbWeight"))
	s.Set("weightDistribution", nz(r, "WeightDistribution"))
	s.Set("powertrainId", int64(nz(r, "PowertrainID")))
	s.Set("isLiveryLocked", false)
	s.Set("isUnicorn", !r.IsNull("IsUnicorn") && r.Int("IsUnicorn") != 0)
	s.Set("isReward", false)
	s.Set("wasOriginallyFromAuctionHouse", false)
	s.Set("wasOriginallyGifted", true)
	s.Set("speedRating", nz(r, "SpeedRating"))
	s.Set("accelerationRating", nz(r, "AccelerationRating"))
	s.Set("launchRating", nz(r, "LaunchRating"))
	s.Set("brakingRating", nz(r, "BrakingRating"))
	s.Set("handlingRating", nz(r, "HandlingRating"))
	s.Set("versionedLiveryId", int64(0))
	s.Set("versionedTuneId", int64(0))
	return s, nil
}

// MakeModel is (MakeID, ModelId) of a car.
func (c *CarDb) MakeModel(carID int64) (int64, int64, bool, error) {
	r, err := c.Row(carID)
	if err != nil || r == nil {
		return 0, 0, false, err
	}
	return r.Int("MakeID"), r.Int("ModelId"), true, nil
}

// PoolCar is (carId, classId, performanceIndex, top speed m/s) of a car, for synthesized leaderboard rows.
type PoolCar struct {
	ID, Class int64
	PI, Top   float64
}

// FakePool lists every car except traffic vehicles and hidden TF variants.
func (c *CarDb) FakePool() ([]PoolCar, error) {
	rows, err := c.DB.Query(`select Id, ClassID, PerformanceIndex, "topspeed-mph" as top from Data_Car where MediaName not like 'PG\_%' escape '\' and MediaName not like '%TF\_%' escape '\'`)
	if err != nil {
		return nil, err
	}
	out := make([]PoolCar, len(rows))
	for i := range rows {
		r := &rows[i]
		out[i] = PoolCar{r.Int("Id"), r.Int("ClassID"), nz(r, "PerformanceIndex"), nz(r, "top") * 0.44704}
	}
	return out, nil
}

// RouteInfo is (length in metres, start-to-finish distance in metres) of a track's race route, ok=false when unknown.
func (c *CarDb) RouteInfo(trackID int64) (length, startFinish float64, ok bool, err error) {
	c.mu.Lock()
	hit, cached := c.route[trackID]
	c.mu.Unlock()
	if !cached {
		r, e := c.DB.One("SELECT length_m, start_finish_m FROM routes WHERE track_id=?", trackID)
		if e != nil {
			return 0, 0, false, e
		}
		if r != nil {
			hit = &[2]float64{r.FloatAt(0), r.FloatAt(1)}
		}
		c.mu.Lock()
		c.route[trackID] = hit
		c.mu.Unlock()
	}
	if hit == nil {
		return 0, 0, false, nil
	}
	return hit[0], hit[1], true, nil
}

// RouteLength is the route length in metres (ok=false when unknown).
func (c *CarDb) RouteLength(trackID int64) (float64, bool) {
	l, _, ok, _ := c.RouteInfo(trackID)
	return l, ok
}

// IsCircuit says whether the track is a closed circuit.
func (c *CarDb) IsCircuit(trackID int64) bool {
	_, sf, ok, _ := c.RouteInfo(trackID)
	return ok && sf < 150
}
