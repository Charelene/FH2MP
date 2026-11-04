// Package xpaudit decides which SubmitXpScore awards count (= server/xp_audit.py: judge, suspects and the limits; the one-off log audit CLI of
// October stays in Python). SubmitXpScore carries only the amount, so where an award came from has to be inferred: an award above MaxAward, not
// finite or not positive is rejected (stored, never counted); everything else counts, and Suspects finds the amounts worth a closer look.
package xpaudit

import (
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"

	"fh2mp/internal/pyfmt"
)

// Verdicts.
const (
	OK       = "ok"
	Rejected = "rejected"
)

// Tunables.
const (
	RareUsers       = 10  // an amount fewer accounts than this ever received is "rare"
	Repeats         = 20  // ... and one account receiving it this often is a suspect
	BeforePlausible = 5e6 // a balance carried into the log above this is reported
)

func envFloat(name string, def float64) float64 {
	if v := os.Getenv(name); v != "" {
		if f, err := strconv.ParseFloat(v, 64); err == nil {
			return f
		}
	}
	return def
}

var (
	// XPScale multiplies online race XP per km (env FH2MP_XP_SCALE, default 10), and so is the largest honest award.
	XPScale = envFloat("FH2MP_XP_SCALE", 10)
	// MaxAward is the largest honest award (env FH2MP_XP_MAX).
	MaxAward = envFloat("FH2MP_XP_MAX", 56000*XPScale)
	// HourBudget is reported only, until real play shows where honest hours end (env FH2MP_XP_HOUR_BUDGET).
	HourBudget = envFloat("FH2MP_XP_HOUR_BUDGET", MaxAward*3)
)

// Judge returns (verdict, reason) for one award.
func Judge(xp float64) (string, string) {
	if math.IsNaN(xp) || math.IsInf(xp, 0) {
		return Rejected, "not a finite number"
	}
	if xp < 0 {
		return Rejected, "negative"
	}
	if xp > MaxAward {
		return Rejected, fmt.Sprintf("above the largest possible award (%s)", pyfmt.FmtG(MaxAward))
	}
	return OK, ""
}

// Award is one counted award.
type Award struct {
	Xuid int64
	XP   float64
}

// Suspect is an amount one account received suspiciously often although few accounts ever got it.
type Suspect struct {
	Amount   float64
	Times    int
	Accounts int
}

// Suspects finds, per account, the rare amounts it repeats.
func Suspects(awards []Award, rareUsers, repeats int) map[int64][]Suspect {
	byAmount := map[float64]map[int64]int{}
	var order []float64
	for _, a := range awards {
		m := byAmount[a.XP]
		if m == nil {
			m = map[int64]int{}
			byAmount[a.XP] = m
			order = append(order, a.XP)
		}
		m[a.Xuid]++
	}
	out := map[int64][]Suspect{}
	for _, amount := range order {
		users := byAmount[amount]
		if len(users) >= rareUsers {
			continue
		}
		xs := make([]int64, 0, len(users))
		for x := range users {
			xs = append(xs, x)
		}
		sort.Slice(xs, func(i, j int) bool { return xs[i] < xs[j] })
		for _, x := range xs {
			if n := users[x]; n >= repeats {
				out[x] = append(out[x], Suspect{amount, n, len(users)})
			}
		}
	}
	return out
}
