package ugc

import (
	"testing"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
)

// A design the server handed out with a Drivatar (or the featured tile) is not downloaded when the game loads it; a storefront download still counts.
func TestServedDesignsAreNotDownloads(t *testing.T) {
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	s := New(d)
	file := func(owner int64) (int64, int64) {
		vid, err := s.VersionedCreate("livery", owner, "L", nil, nil, 1, nil, nil, []byte("data"))
		if err != nil {
			t.Fatal(err)
		}
		fid, err := s.FileCreate(FileSpec{Kind: "livery", Owner: owner, Title: "L", Type: 2, VersionedID: &vid, Preview: []byte("p")})
		if err != nil {
			t.Fatal(err)
		}
		return vid, fid
	}
	downloads := func(fid int64) int64 {
		f, err := s.FileGet(fid)
		if err != nil || f == nil {
			t.Fatal(err)
		}
		return f.Downloads
	}
	v1, f1 := file(100)
	_, f2 := file(100)
	if err := s.MarkServed(200, []int64{v1, 999}); err != nil {
		t.Fatal(err)
	}
	if err := s.MarkServed(200, []int64{v1}); err != nil { // handed out again: just restarts the window
		t.Fatal(err)
	}
	if ok, err := s.RecordAction(f1, 200, "download", "downloads"); err != nil || ok {
		t.Fatalf("download of a served design counted: %v %v", ok, err)
	}
	if ok, err := s.RecordAction(f1, 300, "download", "downloads"); err != nil || !ok { // served to someone else
		t.Fatalf("another player's download not counted: %v %v", ok, err)
	}
	if ok, err := s.RecordAction(f2, 200, "download", "downloads"); err != nil || !ok { // a design that was not served
		t.Fatalf("storefront download not counted: %v %v", ok, err)
	}
	if ok, err := s.RecordAction(f1, 200, "like", "likes"); err != nil || !ok { // likes are never held back
		t.Fatalf("like not counted: %v %v", ok, err)
	}
	if _, err := d.Exec("UPDATE ugc_actions SET at = at - ? WHERE action = 'served'", float64(ServedWindow+1)); err != nil {
		t.Fatal(err)
	}
	if ok, err := s.RecordAction(f1, 200, "download", "downloads"); err != nil || !ok { // long after: a real download
		t.Fatalf("download after the window not counted: %v %v", ok, err)
	}
	if downloads(f1) != 2 || downloads(f2) != 1 {
		t.Fatalf("downloads %d %d, want 2 1", downloads(f1), downloads(f2))
	}
}
