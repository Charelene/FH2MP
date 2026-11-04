// Command fh2mp-lsp is the LSP (= python3 server/fh2mp_lsp.py [port]).
package main

import (
	"fmt"
	"os"
	"strconv"

	"fh2mp/internal/lsp"
)

func main() {
	port := 5080
	if len(os.Args) > 1 {
		p, err := strconv.Atoi(os.Args[1])
		if err != nil {
			fmt.Fprintln(os.Stderr, "fh2mp-lsp: bad port:", os.Args[1])
			os.Exit(2)
		}
		port = p
	}
	c := lsp.New()
	if err := c.Setup(""); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	bind := os.Getenv("FH2MP_LSP_BIND")
	if bind == "" {
		bind = "127.0.0.1"
	}
	c.StartSampler()
	if err := c.Serve(bind, port); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
