package notifstore

import (
	"fmt"
	"os"
	"testing"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
	"fh2mp/internal/pyjson"
)

var ids map[int64]int64

// clean replaces ids (the sequence starts at the current time) by the order they were first seen in, like the Python script.
func clean(n *Notification) pyjson.Obj {
	label, ok := ids[n.ID]
	if !ok {
		label = int64(len(ids) + 1)
		ids[n.ID] = label
	}
	out := pyjson.Obj{}
	for _, p := range n.JSON() {
		switch p.K {
		case "sent":
			continue
		case "id":
			p.V = label
		case "params":
			o := append(pyjson.Obj(nil), p.V.(pyjson.Obj)...)
			if g, has := o.Get("GiftId"); has && g == (n.ID&0x7FFFFFFF) {
				o.Set("GiftId", fmt.Sprintf("id%d", label))
			}
			p.V = o
		}
		out = append(out, p)
	}
	return out
}

func cleanList(l []*Notification) []any {
	out := make([]any, len(l))
	for i, n := range l {
		out[i] = clean(n)
	}
	return out
}

func TestMatchesPython(t *testing.T) {
	raw, err := os.ReadFile("../../testdata/notif.json")
	if err != nil {
		t.Skip(err)
	}
	want, err := pyjson.Parse(string(raw))
	if err != nil {
		t.Fatal(err)
	}
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	s := New(d)
	ids = map[int64]int64{}
	var got []any
	must := func(n *Notification, err error) *Notification {
		if err != nil {
			t.Fatal(err)
		}
		return n
	}
	ok := func(err error) {
		if err != nil {
			t.Fatal(err)
		}
	}
	car := int64(1331)
	first := must(s.Add("hello", "", 0x602, pyjson.Obj{{K: "a", V: int64(1)}, {K: "b", V: "x"}}, 0, nil, false))
	second := must(s.Add("gift for 5", "sra1", 0x8502, pyjson.Obj{{K: "Z", V: int64(2)}}, 5, nil, true))
	third := must(s.Add("car note", "", 0x602, nil, 0, &car, false))
	fourth := must(s.Add("gift keeps id", "", 0x8502, pyjson.Obj{{K: "GiftId", V: int64(77)}}, 5, nil, true))
	got = append(got, clean(first), clean(second), clean(third), clean(fourth))
	vis, err := s.Visible(5)
	ok(err)
	got = append(got, cleanList(vis))
	ok(s.MarkRead(5, first.ID))
	for _, c := range [][2]int64{{5, second.ID}, {5, second.ID}, {6, second.ID}} {
		b, err := s.Claim(c[0], c[1])
		ok(err)
		got = append(got, b)
	}
	ok(s.Clear(5, first.ID))
	for _, x := range []int64{5, 6} {
		v, err := s.Visible(x)
		ok(err)
		got = append(got, cleanList(v))
	}
	n99 := int64(99)
	ok(s.SetCarID(first.ID, &n99))
	all, err := s.All()
	ok(err)
	got = append(got, cleanList(all))
	n3, err := s.Get(third.ID)
	ok(err)
	got = append(got, clean(n3))
	nx, err := s.Get(999)
	ok(err)
	if nx != nil {
		t.Fatal("999 exists")
	}
	got = append(got, nil)
	for i := 0; i < 2; i++ {
		b, err := s.Delete(second.ID)
		ok(err)
		got = append(got, b)
	}
	c, err := s.Count()
	ok(err)
	got = append(got, c)
	a, _ := pyjson.Dumps(got, pyjson.Opts{})
	b, _ := pyjson.Dumps(want, pyjson.Opts{})
	if a != b {
		t.Fatalf("differs\n got %s\nwant %s", a, b)
	}
}
