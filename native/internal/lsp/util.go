package lsp

import (
	"fh2mp/internal/scoreboard"
	"fmt"
	"strconv"
	"strings"
)

// parseIntBase0 is int(text, 0): an optional sign, then 0x / 0o / 0b prefixes or decimal, underscores between digits allowed.
func parseIntBase0(s string) (int64, error) {
	t := strings.TrimSpace(s)
	if t == "" {
		return 0, fmt.Errorf("empty")
	}
	neg := false
	if t[0] == '+' || t[0] == '-' {
		neg = t[0] == '-'
		t = t[1:]
	}
	base := 10
	low := strings.ToLower(t)
	switch {
	case strings.HasPrefix(low, "0x"):
		base, t = 16, t[2:]
	case strings.HasPrefix(low, "0o"):
		base, t = 8, t[2:]
	case strings.HasPrefix(low, "0b"):
		base, t = 2, t[2:]
	case len(t) > 1 && t[0] == '0':
		// a leading zero is only valid for zero itself in base 0
		if strings.Trim(t, "0_") != "" {
			return 0, fmt.Errorf("leading zero")
		}
	}
	t = strings.ReplaceAll(t, "_", "")
	n, err := strconv.ParseInt(t, base, 64)
	if err != nil {
		u, err2 := strconv.ParseUint(t, base, 64)
		if err2 != nil {
			return 0, err
		}
		n = int64(u)
	}
	if neg {
		n = -n
	}
	return n, nil
}

// iso formats a time like the Python helper (UTC, whole seconds).
func iso(t float64) string { return scoreboard.Iso(t) }

func b2s(b bool) string {
	if b {
		return "True"
	}
	return "False"
}

func hex(x int64) string { return fmt.Sprintf("%#x", x) }
