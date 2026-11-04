// Command fh2mp-server is the game server (= python3 server/fh2mp_server.py [port] [logfile] [--roadtrip] [--carmeet]).
package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"

	"fh2mp/internal/gamenet"
)

func main() {
	var args []string
	cfg := gamenet.Config{Port: 5000}
	for _, a := range os.Args[1:] {
		switch {
		case a == "--roadtrip":
			cfg.Roadtrip = true
		case a == "--carmeet":
			cfg.Carmeet = true
		case strings.HasPrefix(a, "--"):
		default:
			args = append(args, a)
		}
	}
	if len(args) > 0 {
		p, err := strconv.Atoi(args[0])
		if err != nil {
			fmt.Fprintln(os.Stderr, "fh2mp-server: bad port:", args[0])
			os.Exit(2)
		}
		cfg.Port = p
	}
	if len(args) > 1 {
		cfg.LogPath = args[1]
	}
	s, err := gamenet.New(cfg)
	if err != nil {
		fmt.Fprintln(os.Stderr, "fh2mp-server:", err)
		os.Exit(1)
	}
	if err := s.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "fh2mp-server:", err)
		os.Exit(1)
	}
}
