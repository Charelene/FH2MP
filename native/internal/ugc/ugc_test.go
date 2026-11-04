package ugc

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"reflect"
	"testing"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
)

type opt struct {
	In  string  `json:"in"`
	Out *string `json:"out"`
}

type fixture struct {
	Jpeg []struct {
		In  string  `json:"in"`
		Out *[2]int `json:"out"`
	} `json:"jpeg"`
	Chunks []opt                      `json:"chunks"`
	Strip  []struct{ In, Out string } `json:"strip"`
	Livery []struct {
		In  string `json:"in"`
		Out *struct {
			Name      string `json:"name"`
			CarID     int64  `json:"car_id"`
			Layers    int    `json:"layers"`
			Container string `json:"container"`
			Payload   string `json:"payload"`
		} `json:"out"`
	} `json:"livery"`
	Group []struct {
		In  string `json:"in"`
		Out *struct {
			Name    string `json:"name"`
			Layers  int    `json:"layers"`
			Payload string `json:"payload"`
		} `json:"out"`
	} `json:"group"`
	Pack      []struct{ Data, Packed string } `json:"pack"`
	UnpackBad []struct {
		In  string `json:"in"`
		Err bool   `json:"err"`
	} `json:"unpack_bad"`
	UnpackPlain []struct{ In, Out string } `json:"unpack_plain"`
	Utf8        []struct{ In, Out string } `json:"utf8"`
	Steps       []struct {
		S     string          `json:"s"`
		R     json.RawMessage `json:"r"`
		Limit *string         `json:"limit"`
	} `json:"steps"`
}

func load(t *testing.T) *fixture {
	raw, err := os.ReadFile("../../testdata/ugc.json")
	if err != nil {
		t.Skip(err)
	}
	var f fixture
	if err := json.Unmarshal(raw, &f); err != nil {
		t.Fatal(err)
	}
	return &f
}

func unhex(s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		panic(err)
	}
	return b
}

func TestPure(t *testing.T) {
	f := load(t)
	for i, c := range f.Jpeg {
		w, h, ok := JpegSize(unhex(c.In))
		if (c.Out != nil) != ok || (ok && *c.Out != [2]int{w, h}) {
			t.Errorf("jpeg %d: got %d,%d,%v want %v", i, w, h, ok, c.Out)
		}
	}
	for i, c := range f.Chunks {
		got := LiveryChunk(unhex(c.In))
		if (c.Out == nil) != (got == nil) || (c.Out != nil && hex.EncodeToString(got) != *c.Out) {
			t.Errorf("chunk %d differs (python ok=%v, go ok=%v)", i, c.Out != nil, got != nil)
		}
	}
	for i, c := range f.Strip {
		if got := hex.EncodeToString(StripLiveryPrefix(unhex(c.In))); got != c.Out {
			t.Errorf("strip %d: got %s want %s", i, got, c.Out)
		}
	}
	for i, c := range f.Livery {
		got := ParseLiveryUpload(unhex(c.In))
		switch {
		case (c.Out == nil) != (got == nil):
			t.Errorf("livery %d: python ok=%v, go ok=%v", i, c.Out != nil, got != nil)
		case got != nil && (got.Name != c.Out.Name || got.CarID != c.Out.CarID || got.Layers != c.Out.Layers ||
			hex.EncodeToString(got.Container) != c.Out.Container || hex.EncodeToString(got.Payload) != c.Out.Payload):
			t.Errorf("livery %d differs", i)
		}
	}
	for i, c := range f.Group {
		got := ParseLayergroupUpload(unhex(c.In))
		switch {
		case (c.Out == nil) != (got == nil):
			t.Errorf("group %d: python ok=%v, go ok=%v", i, c.Out != nil, got != nil)
		case got != nil && (got.Name != c.Out.Name || got.Layers != c.Out.Layers || hex.EncodeToString(got.Payload) != c.Out.Payload):
			t.Errorf("group %d differs", i)
		}
	}
	for i, c := range f.Pack {
		data := unhex(c.Data)
		if got, err := Unpack(unhex(c.Packed)); err != nil || !bytes.Equal(got, data) {
			t.Errorf("pack %d: python's packed value does not unpack (%v)", i, err)
		}
		// what Go writes must read back, and be one of the two codecs
		mine := Pack(data)
		if got, err := Unpack(mine); err != nil || !bytes.Equal(got, data) {
			t.Errorf("pack %d: own round trip failed (%v)", i, err)
		}
		if len(data) > 0 && (len(mine) < 4 || !bytes.Equal(mine[:3], Magic) || mine[3] > 1) {
			t.Errorf("pack %d: bad header", i)
		}
		if p := unhex(c.Packed); len(p) > 3 && p[3] == 1 && mine[3] != 1 {
			t.Errorf("pack %d: python compressed it, go did not", i)
		}
	}
	for i, c := range f.UnpackBad {
		_, err := Unpack(unhex(c.In))
		var le *LimitError
		if errors.As(err, &le) != c.Err {
			t.Errorf("unpack_bad %d: err=%v want error %v", i, err, c.Err)
		}
	}
	for i, c := range f.UnpackPlain {
		got, err := Unpack(unhex(c.In))
		if err != nil || hex.EncodeToString(got) != c.Out {
			t.Errorf("unpack_plain %d: %x %v", i, got, err)
		}
	}
}

func TestDecodeReplace(t *testing.T) {
	f := load(t)
	for i, c := range f.Utf8 {
		if got := pyfmt.DecodeReplace(unhex(c.In)); got != c.Out {
			t.Fatalf("utf8 %d (%s): got %q want %q", i, c.In, got, c.Out)
		}
	}
}

// blobs: Go's packed value is read by Python's lzma too; this is checked by tools/golden/check_ugc_pack.py on the file written here.
func TestWritePackedForPython(t *testing.T) {
	dir := os.Getenv("FH2MP_UGC_PACK_DIR")
	if dir == "" {
		t.Skip("set FH2MP_UGC_PACK_DIR to write the packed values Python should read")
	}
	for i, d := range [][]byte{[]byte("hello hello hello hello hello hello"), bytes.Repeat([]byte("abc"), 5000), make([]byte, 100000)} {
		p := Pack(d)
		if err := os.WriteFile(dir+"/"+string(rune('a'+i))+".packed", p, 0o644); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(dir+"/"+string(rune('a'+i))+".raw", d, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

// ---- the store

type run struct {
	t     *testing.T
	f     *fixture
	n     int
	store *Store
}

func sha(b []byte) map[string]any {
	h := sha256.Sum256(b)
	return map[string]any{"len": len(b), "sha": hex.EncodeToString(h[:])}
}

func (r *run) check(name string, got any, err error) {
	r.t.Helper()
	if r.n >= len(r.f.Steps) {
		r.t.Fatalf("step %s: no more expected steps", name)
	}
	want := r.f.Steps[r.n]
	r.n++
	if want.S != name {
		r.t.Fatalf("step %d is %s in Python and %s in Go", r.n, want.S, name)
	}
	var le *LimitError
	if errors.As(err, &le) {
		if want.Limit == nil || *want.Limit != le.Msg {
			r.t.Fatalf("step %s: limit error %q, python %v / %s", name, le.Msg, want.Limit, want.R)
		}
		return
	}
	if err != nil {
		r.t.Fatalf("step %s: %v", name, err)
	}
	if want.Limit != nil {
		r.t.Fatalf("step %s: python raised %q, go did not", name, *want.Limit)
	}
	b, _ := json.Marshal(got)
	var g, w any
	json.Unmarshal(b, &g)
	json.Unmarshal(want.R, &w)
	if !reflect.DeepEqual(g, w) {
		r.t.Fatalf("step %s differs\n got %s\nwant %s", name, b, want.R)
	}
}

func ptrv(p *int64) any {
	if p == nil {
		return nil
	}
	return *p
}

func pub(f *File, preview bool) map[string]any {
	if f == nil {
		return nil
	}
	m := map[string]any{"id": f.ID, "kind": f.Kind, "type": f.Type, "owner": f.Owner, "title": f.Title, "description": f.Description,
		"payload_size": f.PayloadSize, "versioned_id": ptrv(f.VersionedID), "blob_id": ptrv(f.BlobID), "car_id": ptrv(f.CarID), "make_id": ptrv(f.MakeID),
		"model_id": ptrv(f.ModelID), "class_id": ptrv(f.ClassID), "keyword1": f.Keyword1, "keyword2": f.Keyword2, "env": ptrv(f.Env), "track": ptrv(f.Track),
		"pi": ptrv(f.PI), "downloads": f.Downloads, "likes": f.Likes, "uses": f.Uses, "views": f.Views, "removed": f.Removed}
	if preview {
		m["preview"] = sha(f.Preview)
	}
	return m
}

func pubs(fs []*File, preview bool) []any {
	out := []any{}
	for _, f := range fs {
		out = append(out, pub(f, preview))
	}
	return out
}

func strp(p *string) any {
	if p == nil {
		return nil
	}
	return *p
}

func vers(v *Versioned) any {
	if v == nil {
		return nil
	}
	return map[string]any{"id": v.ID, "kind": v.Kind, "car_id": ptrv(v.CarID), "name": v.Name, "creator": v.Creator, "layers": v.Layers,
		"data": sha(v.Data), "thumb": sha(v.Thumb), "thumb_small": sha(v.ThumbSmall), "parts": strp(v.Parts), "tuning": strp(v.Tuning), "refs": v.Refs}
}

func chunkOf(c *BlobChunk) any {
	if c == nil {
		return nil
	}
	return []any{sha(c.Data), c.Last, c.Total}
}

func i64(v int64) *int64 { return &v }

func TestStoreMatchesPython(t *testing.T) {
	f := load(t)
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	for _, p := range []struct {
		x   int64
		tag string
	}{{100, "Alice"}, {200, "Bob"}, {300, "carol"}} {
		if _, err := d.Exec("INSERT INTO players (xuid, gamertag, first_seen, last_seen) VALUES (?,?,?,?)", p.x, p.tag, 1.0, 1.0); err != nil {
			t.Fatal(err)
		}
	}
	s := New(d)
	r := &run{t: t, f: f, store: s}
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	write := func(name string, id, off int64, data []byte, owner int64, fin func([]byte) []byte) {
		n, ok, err := s.BlobWrite(id, off, data, owner, fin)
		var got any
		if ok {
			got = n
		}
		r.check(name, got, err)
	}
	read := func(name string, id, off, size int64) {
		c, err := s.BlobRead(id, off, size)
		r.check(name, chunkOf(c), err)
	}

	b1, err := s.BlobCreate(100, "a.bin", 10)
	r.check("blob_create", b1, err)
	write("write0", b1, 0, []byte("01234"), 100, nil)
	read("read_partial", b1, 0, 0)
	write("write1", b1, 5, []byte("56789"), 100, nil)
	read("read_all", b1, 0, 0)
	read("read_mid", b1, 2, 4)
	read("read_tail", b1, 8, 100)
	read("read_past", b1, 50, 5)
	read("read_unknown", 999999, 0, 5)
	write("write_other_owner", b1, 0, []byte("x"), 200, nil)
	write("write_beyond", b1, 8, []byte("xxxxx"), 100, nil)
	write("write_neg", b1, -1, []byte("x"), 100, nil)
	_, err = s.BlobCreate(100, "big", MaxBlobSize+1)
	r.check("create_big", nil, err)
	_, err = s.BlobCreate(100, "neg", -1)
	r.check("create_neg", nil, err)
	b2, err := s.BlobCreate(100, "gap.bin", 10)
	r.check("blob_gap", b2, err)
	write("write_gap", b2, 6, []byte("abcd"), 100, nil)
	read("read_gap", b2, 0, 0)
	for i := 0; i < 32; i++ {
		_, err := s.BlobCreate(300, "q"+string(rune('0'+i/10))+string(rune('0'+i%10)), MaxBlobSize)
		must(err)
	}
	_, err = s.BlobCreate(300, "one-too-many", MaxBlobSize)
	r.check("quota", nil, err)
	byName := func(name string, owner int64, n string) {
		id, data, err := s.BlobByName(owner, n)
		var got any
		if id != 0 {
			got = []any{id, sha(data)}
		}
		r.check(name, got, err)
	}
	byName("by_name", 100, "a.bin")
	byName("by_name_none", 100, "nope")

	p1, err := s.BlobCreate(100, "", 3)
	r.check("blob_photo", p1, err)
	fph, err := s.FileCreate(FileSpec{Kind: "photo", Owner: 100, Title: "A photo", Description: "desc", Type: 5, PayloadSize: 6, Preview: []byte("prev"),
		BlobID: i64(p1), CarID: i64(11), MakeID: i64(2)})
	r.check("file_photo", fph, err)
	cb, err := s.ContentBlob(fph)
	r.check("content_blob_photo", cb, err)
	cb, err = s.ContentBlob(b1)
	r.check("content_blob_plain", cb, err)
	ph1, err := s.IsPhoto(fph)
	must(err)
	ph2, err := s.IsPhoto(b1)
	r.check("is_photo", []bool{ph1, ph2}, err)
	anyClass := func(q SearchSpec) SearchSpec {
		if q.Limit == 0 {
			q.Limit = 50
		}
		return q
	}
	search := func(name string, q SearchSpec) {
		fs, err := s.Search(anyClass(q))
		r.check(name, pubs(fs, false), err)
	}
	search("search_incomplete", SearchSpec{Kind: "photo"})
	fs, err := s.Popular("photo", 10, 0)
	r.check("popular_incomplete", pubs(fs, false), err)
	double := func(x []byte) []byte { return append(append([]byte(nil), x...), x...) }
	write("photo_chunk", p1, 0, []byte("abc"), 100, double)
	write("photo_chunk_again", p1, 0, []byte("abc"), 100, double)
	read("photo_blob", p1, 0, 0)
	ff, err := s.FileGet(fph)
	r.check("photo_file_after", pub(ff, true), err)
	search("search_complete", SearchSpec{Kind: "photo"})

	v1, err := s.VersionedCreate("livery", 100, "Livery A", i64(1331), bytes.Repeat([]byte("thumbdata"), 30), 7, nil, nil, bytes.Repeat([]byte("liverydata"), 200))
	r.check("v_create", v1, err)
	v2, err := s.VersionedCreate("tune", 200, "Tune", i64(77), []byte{}, -3, pyjson.Obj{{K: "engine", V: []int{1, 2}}, {K: "x", V: "y"}}, pyjson.Obj{{K: "a", V: 1.5}}, nil)
	r.check("v_tune", v2, err)
	vget := func(name string, id int64) {
		v, err := s.VersionedGet(id)
		r.check(name, vers(v), err)
	}
	vget("v_get", v1)
	vget("v_get_tune", v2)
	vget("v_get_none", 99999)
	n, err := s.VersionedSetData(v1, []byte("new"), 200)
	r.check("v_set_data_wrong", n, err)
	n, err = s.VersionedSetData(v1, bytes.Repeat([]byte("new"), 100), 100)
	r.check("v_set_data", n, err)
	n, err = s.VersionedSetData(v1, []byte("anyone"), 0)
	r.check("v_set_data_any", n, err)
	n, err = s.VersionedSetThumbs(v1, bytes.Repeat([]byte("big"), 50), []byte("small"), 100)
	r.check("v_set_thumbs", n, err)
	n, err = s.VersionedSetThumbs(v1, []byte("big"), []byte("small"), 5)
	r.check("v_set_thumbs_wrong", n, err)
	n, err = s.VersionedSetLayers(v1, -5, 100)
	r.check("v_set_layers", n, err)
	n, err = s.VersionedSetLayers(v1, 12, 100)
	r.check("v_set_layers2", n, err)
	must(s.VersionedRef(v1, 2))
	must(s.VersionedRef(v2, -10))
	a, _ := s.VersionedGet(v1)
	b, _ := s.VersionedGet(v2)
	r.check("v_get_after", []any{vers(a), vers(b)}, nil)

	long := func(c string, n int) string { return string(bytes.Repeat([]byte(c), n)) }
	var ids []int64
	for _, sp := range []FileSpec{
		{Kind: "livery", Owner: 100, Title: "Red Racer", Description: "fast red car", Type: 1, VersionedID: i64(v1), CarID: i64(1331), MakeID: i64(5), ModelID: i64(50), ClassID: i64(3), Keyword1: 2, Keyword2: 4, Env: i64(1), Track: i64(9), PI: i64(500)},
		{Kind: "livery", Owner: 200, Title: "Blue Bomber", Description: "blue and fast", Type: 1, CarID: i64(1331), MakeID: i64(5), ModelID: i64(51), ClassID: i64(4)},
		{Kind: "tune", Owner: 200, Title: "Drift tune", Description: "for sliding", Type: 2, VersionedID: i64(v2), CarID: i64(77), MakeID: i64(6), ClassID: i64(3)},
		{Kind: "livery", Owner: 100, Title: long("x", 60), Description: long("d", 600), Type: 1, CarID: i64(2000)},
		{Kind: "layergroup", Owner: 300, Title: "Flames", Description: "vinyl", Type: 3},
	} {
		sp.PayloadSize, sp.Preview = 10, bytes.Repeat([]byte("preview"), 20)
		id, err := s.FileCreate(sp)
		r.check("file_create", id, err)
		ids = append(ids, id)
	}
	ff, err = s.FileGet(ids[0])
	r.check("file_get", pub(ff, true), err)
	ff, err = s.FileGet(99999)
	r.check("file_get_none", pub(ff, true), err)
	fs, err = s.Files([]int64{ids[2], 12345, ids[0]})
	r.check("files", pubs(fs, true), err)
	fs, err = s.Mine(100, 1, 10)
	r.check("mine", pubs(fs, false), err)
	fs, err = s.Mine(100, 1, 1)
	r.check("mine_limit", pubs(fs, false), err)
	for _, c := range []struct {
		id  int64
		col string
	}{{ids[1], "likes"}, {ids[1], "likes"}, {ids[1], "likes"}, {ids[0], "uses"}, {ids[0], "downloads"}, {ids[0], "views"}} {
		must(s.Bump(c.id, c.col))
	}
	search("search_all", SearchSpec{Kind: "livery"})
	search("search_make", SearchSpec{Kind: "livery", Make: 5})
	search("search_model", SearchSpec{Kind: "livery", Model: 51})
	search("search_class", SearchSpec{Kind: "livery", Class: i64(3)})
	search("search_class0", SearchSpec{Kind: "livery", Class: i64(0)})
	search("search_kw", SearchSpec{Kind: "livery", Kw1: 2, Kw2: 4, Env: 1, Track: 9})
	search("search_text", SearchSpec{Kind: "livery", Text: "FAST"})
	search("search_text_pct", SearchSpec{Kind: "livery", Text: "%"})
	search("search_gamertag", SearchSpec{Kind: "livery", Gamertag: "ALICE"})
	search("search_gamertag_none", SearchSpec{Kind: "livery", Gamertag: "nobody"})
	search("search_limit", SearchSpec{Kind: "livery", Limit: 2})
	fs, err = s.Curated(1, 1331, 10)
	r.check("curated", pubs(fs, false), err)
	fs, err = s.Curated(1, 0, 10)
	r.check("curated_any", pubs(fs, false), err)
	fs, err = s.Popular("livery", 10, 0)
	r.check("popular", pubs(fs, false), err)
	fs, err = s.Popular("livery", 10, 9e9)
	r.check("popular_since", pubs(fs, false), err)
	fs, err = s.Files(ids)
	stars := []int{}
	for _, f := range fs {
		stars = append(stars, f.Stars())
	}
	r.check("stars", stars, err)
	act := func(name string, fid, x int64, action, col string) {
		ok, err := s.RecordAction(fid, x, action, col)
		r.check(name, ok, err)
	}
	act("action_like", ids[0], 200, "like", "likes")
	act("action_like_repeat", ids[0], 200, "like", "likes")
	act("action_download", ids[0], 200, "download", "downloads")
	act("action_own", ids[0], 100, "like", "likes")
	act("action_unknown", 98765, 200, "like", "likes")
	ff, err = s.FileGet(ids[0])
	r.check("after_actions", pub(ff, true), err)
	fo1, _ := s.FileOfVersioned(v1)
	fo2, _ := s.FileOfVersioned(555)
	var fo2v any
	if fo2 != 0 {
		fo2v = fo2
	}
	r.check("file_of_versioned", []any{fo1, fo2v}, nil)
	title, desc, hack := long("T", 50), "new", "hack"
	must(s.Retitle(ids[1], 200, &title, &desc))
	ff, err = s.FileGet(ids[1])
	r.check("retitle", pub(ff, true), err)
	must(s.Retitle(ids[1], 100, &hack, nil))
	ff, err = s.FileGet(ids[1])
	r.check("retitle_wrong", pub(ff, true), err)
	n, err = s.Remove(ids[1], 100)
	r.check("remove_wrong", n, err)
	n, err = s.Remove(ids[1], 200)
	r.check("remove", n, err)
	ff, err = s.FileGet(ids[1])
	r.check("removed_get", pub(ff, true), err)
	c0, _ := s.Count("")
	c1, _ := s.Count("livery")
	c2, _ := s.Count("tune")
	c3, _ := s.Count("nothing")
	r.check("count", []int64{c0, c1, c2, c3}, nil)

	for _, q := range []struct {
		sql string
		a   []any
	}{{"UPDATE ugc_blobs SET data=? WHERE id=?", []any{bytes.Repeat([]byte("raw legacy bytes"), 40), b2}},
		{"UPDATE ugc_versioned SET thumb=? WHERE id=?", []any{bytes.Repeat([]byte("legacy thumb"), 30), v2}},
		{"UPDATE ugc_files SET preview=? WHERE id=?", []any{[]byte("short"), ids[0]}}} {
		_, err := d.Exec(q.sql, q.a...)
		must(err)
	}
	read("legacy_read", b2, 0, 0)
	before, after, err := s.Recompress()
	r.check("recompress", []any{before, after > 0}, err)
	before, after, err = s.Recompress()
	r.check("recompress_again", []int64{before, after}, err)
	c, _ := s.BlobRead(b2, 0, 0)
	v, _ := s.VersionedGet(v2)
	ff, _ = s.FileGet(ids[0])
	r.check("legacy_after", []any{chunkOf(c), sha(v.Thumb), sha(ff.Preview)}, nil)
	// the Python script's `pre` and `good` values are rebuilt from the fixture's strip cases (case 1 is the prefixed blob, case 0 the clean one)
	good, pre := unhex(f.Strip[0].In), unhex(f.Strip[1].In)
	vl, _ := s.VersionedCreate("livery", 100, "Prefixed", i64(1), []byte("t"), 1, nil, nil, pre)
	vl2, _ := s.VersionedCreate("livery", 100, "Fine", i64(1), []byte("t"), 1, nil, nil, good)
	vl3, _ := s.VersionedCreate("tune", 100, "Tune", i64(1), []byte("t"), 1, nil, nil, pre)
	nfix, err := s.RepairLiveries()
	r.check("repair_liveries", nfix, err)
	x1, _ := s.VersionedGet(vl)
	x2, _ := s.VersionedGet(vl2)
	x3, _ := s.VersionedGet(vl3)
	r.check("repaired", []any{sha(x1.Data), sha(x2.Data), sha(x3.Data)}, nil)
	nfix, err = s.RepairLiveries()
	r.check("repair_liveries_again", nfix, err)
	sb, err := s.StoredBytes()
	r.check("stored_bytes_positive", sb > 0, err)
	if r.n != len(f.Steps) {
		t.Fatalf("Go ran %d steps, Python %d", r.n, len(f.Steps))
	}
}
