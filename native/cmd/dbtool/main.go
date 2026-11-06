// Command dbtool opens (and so migrates) a database with the Go db package: dbtool <dsn>.
package main

import (
	"fmt"
	"os"

	"fh2mp/internal/db"
)

func main() {
	d, err := db.Open(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer d.Close()
	r, _ := d.One("select version from schema_version")
	fmt.Println("go ok", r.IntAt(0))
}
