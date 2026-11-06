package pyjson

import (
	"os"
	"testing"
)

func TestIndentLikePython(t *testing.T) {
	want, err := os.ReadFile("../../../../FH2MP/scratch/indent_expect.json")
	if err != nil {
		t.Skip(err)
	}
	v := Obj{{"received", int64(1700000000)}, {"fields", Obj{{"date", "x"}, {"name", "é"}}}, {"failure", "f"}, {"issue", nil},
		{"list", []any{int64(1), []any{int64(2), int64(3)}, Obj{}}}, {"empty", []any{}}, {"eo", Obj{}}}
	got, err := Dumps(v, Opts{Indent: 1})
	if err != nil {
		t.Fatal(err)
	}
	if got != string(want) {
		t.Fatalf("got\n%s\nwant\n%s", got, want)
	}
}
