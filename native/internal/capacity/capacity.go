// Package capacity says how many players a world holds (= server/capacity.py). Only Free Roam grows; events, Road Trip and Car Meet stay at 12.
package capacity

import (
	"os"
	"strconv"
	"strings"
)

// Limits.
const (
	ClientLimit  = 12
	FreeRoamMax  = 75
	FreeRoamType = 2 // Nerd::NetworkLobbyType: 1 RoadTrip, 2 FreeRoam, 3 CarMeet
	Env          = "FH2MP_WORLD_CAPACITY"
)

// FreeRoam is the players in one Free Roam world: FH2MP_WORLD_CAPACITY from 12 to 75, 12 when it is unset or not a number.
func FreeRoam() int {
	n, err := strconv.Atoi(strings.TrimSpace(os.Getenv(Env)))
	if err != nil {
		return ClientLimit
	}
	if n > FreeRoamMax {
		n = FreeRoamMax
	}
	if n < ClientLimit {
		n = ClientLimit
	}
	return n
}

// ForType is by Nerd::NetworkLobbyType.
func ForType(lobbyType int) int {
	if lobbyType == FreeRoamType {
		return FreeRoam()
	}
	return ClientLimit
}

// ForKind is by "freeroam" or "roadtrip".
func ForKind(kind string) int {
	if kind == "freeroam" {
		return FreeRoam()
	}
	return ClientLimit
}

// ForHopper is by matchmaking hopper name.
func ForHopper(hopper string) int {
	if strings.HasPrefix(strings.ToLower(hopper), "freeroam") {
		return FreeRoam()
	}
	return ClientLimit
}
