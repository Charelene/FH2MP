// Package dbtest makes throw-away PostgreSQL databases for tests (never a real one): the admin DSN is FH2MP_TEST_PG_ADMIN_DSN, default the local
// test server of this repository's tooling.
package dbtest

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"os"
	"testing"

	"github.com/jackc/pgx/v5"
)

func adminDSN() string {
	if v := os.Getenv("FH2MP_TEST_PG_ADMIN_DSN"); v != "" {
		return v
	}
	return "postgresql://postgres@127.0.0.1:55432/postgres"
}

// replaceDB swaps the database name of a postgres:// DSN.
func replaceDB(dsn, name string) string {
	cfg, err := pgx.ParseConfig(dsn)
	if err != nil {
		return dsn
	}
	return "postgresql://" + cfg.User + "@" + cfg.Host + ":" + itoa(int(cfg.Port)) + "/" + name
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var d []byte
	for n > 0 {
		d = append([]byte{byte('0' + n%10)}, d...)
		n /= 10
	}
	return string(d)
}

// New creates an empty database (or a copy of `template` when given) and returns its DSN; it is dropped when the test ends. The test is
// skipped when no PostgreSQL answers.
func New(t *testing.T, template string) string {
	t.Helper()
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, adminDSN())
	if err != nil {
		t.Skip("no PostgreSQL for tests:", err)
	}
	b := make([]byte, 4)
	rand.Read(b)
	name := "fh2mp_test_" + hex.EncodeToString(b)
	q := "CREATE DATABASE " + name
	if template != "" {
		q += " TEMPLATE " + template
	}
	if _, err := conn.Exec(ctx, q); err != nil {
		conn.Close(ctx)
		t.Skip("cannot create a test database:", err)
	}
	t.Cleanup(func() {
		c, err := pgx.Connect(ctx, adminDSN())
		if err != nil {
			return
		}
		defer c.Close(ctx)
		c.Exec(ctx, "DROP DATABASE IF EXISTS "+name+" WITH (FORCE)")
	})
	conn.Close(ctx)
	return replaceDB(adminDSN(), name)
}
