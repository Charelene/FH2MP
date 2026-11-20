package avatars

import (
	"errors"
	"strings"
	"testing"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
)

func id(last byte) ItemID {
	var i ItemID
	i[0], i[15] = 0xa0, last
	return i
}

// Every id slot of the layout is read (15 face slots of 16 bytes from 0x0C, 22 entries of 32 bytes from 0x120), and nothing else is.
func TestWornReadsEveryIdSlotAndNothingElse(t *testing.T) {
	m := make([]byte, ManifestSize)
	var want []ItemID
	for s := 0; s < FaceSlots; s++ {
		x := id(byte(s + 1))
		copy(m[FaceAt+FaceStep*s:], x[:])
		want = append(want, x)
	}
	for s := 0; s < AssetSlots; s++ {
		x := id(byte(0x40 + s))
		copy(m[AssetAt+AssetStep*s:], x[:])
		want = append(want, x)
	}
	if AssetAt+AssetStep*AssetSlots != 0x3E0 || FaceAt+FaceStep*FaceSlots != 0xFC {
		t.Fatalf("the slots end at %#x and %#x, the layout says 0xFC and 0x3E0", FaceAt+FaceStep*FaceSlots, AssetAt+AssetStep*AssetSlots)
	}
	// bytes that hold no ids: the head, the colours, each entry's second half, the tail
	junk := func(from, to int) {
		for o := from; o < to; o++ {
			m[o] = 0xEE
		}
	}
	junk(0, FaceAt)
	junk(0xFC, AssetAt)
	for s := 0; s < AssetSlots; s++ {
		junk(AssetAt+AssetStep*s+16, AssetAt+AssetStep*(s+1))
	}
	junk(0x3E0, ManifestSize)
	got := Worn(m)
	if len(got) != len(want) {
		t.Fatalf("%d ids worn, want %d: %v", len(got), len(want), got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("id %d is %s, want %s", i, got[i], want[i])
		}
	}
}

func TestWornSkipsEmptySlotsAndRepeats(t *testing.T) {
	m := make([]byte, ManifestSize)
	x := id(7)
	copy(m[AssetAt:], x[:])
	copy(m[AssetAt+AssetStep*20:], x[:]) // the basic outfit holds what is worn
	m[AssetAt+AssetStep+16] = 0xff       // an entry with a zero id and a mask after it is empty
	if got := Worn(m); len(got) != 1 || got[0] != x {
		t.Fatalf("worn %v, want just %s", got, x)
	}
	if Worn(make([]byte, ManifestSize)) != nil || Worn(make([]byte, ManifestSize-1)) != nil {
		t.Fatal("an empty or short manifest wears something")
	}
}

func TestItemIDText(t *testing.T) {
	x, err := ParseItem("A0000000000000000000000000000001")
	if err != nil || x.String() != "a0000000000000000000000000000001" {
		t.Fatalf("%s, %v", x, err)
	}
	for _, bad := range []string{"", "a000000000000000000000000000001", "a00000000000000000000000000000001", "g0000000000000000000000000000001",
		"00000000000000000000000000000000", "a0000000-0000-0000-0000-00000001"} {
		if _, err := ParseItem(bad); err == nil {
			t.Errorf("%q passed", bad)
		}
	}
}

func TestColourList(t *testing.T) {
	for _, ok := range []string{"", "12 3\n", "0 0\n123456 654321\n"} {
		if err := CheckColours(ok); err != nil {
			t.Errorf("%q refused: %v", ok, err)
		}
	}
	for _, bad := range []string{"12 3", "12  3\n", "12 3\n\n", "1234567 1\n", "-1 2\n", "a b\n", "12 3\r\n", " 12 3\n", strings.Repeat("1 1\n", ColoursMax/4+1)} {
		if err := CheckColours(bad); err == nil {
			t.Errorf("%q passed", bad)
		}
	}
}

func store(t *testing.T) (*Store, *float64) {
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(d.Close)
	if err := d.Do(func(tx *db.Tx) error {
		_, err := tx.Exec("INSERT INTO accounts (xuid, pubkey, gamertag, gamertag_key, created) VALUES (?,?,?,?,?)", 0x9000000000a01, []byte{1}, "Avatar Ann", "avatarann", 1.0)
		return err
	}); err != nil {
		t.Fatal(err)
	}
	clock := 1800000000.0
	s := New(d)
	s.Now = func() float64 { return clock }
	return s, &clock
}

// A version is the save's time, never at or below the one it replaces; a delete leaves no version and keeps the items.
func TestSaveVersionAlwaysRises(t *testing.T) {
	s, clock := store(t)
	const x = 0x9000000000a01
	m := make([]byte, ManifestSize)
	v1, err := s.Save(x, m, "", []byte("png"))
	if err != nil || v1 != 1800000000 {
		t.Fatalf("first save: %v %v", v1, err)
	}
	v2, err := s.Save(x, m, "1 2\n", []byte("png2"))
	if err != nil || v2 <= v1 || v2 > v1+0.002 {
		t.Fatalf("second save at the same time: %v after %v, %v", v2, v1, err)
	}
	*clock -= 50 // the clock went back
	if v3, err := s.Save(x, m, "", []byte("png3")); err != nil || v3 <= v2 {
		t.Fatalf("a save after the clock went back: %v after %v, %v", v3, v2, err)
	}
	*clock += 100
	if v4, err := s.Save(x, m, "", []byte("png4")); err != nil || v4 != *clock {
		t.Fatalf("a later save: %v, want %v (%v)", v4, *clock, err)
	}
	if _, err := s.Save(x, m[:999], "", nil); err == nil {
		t.Fatal("a 999-byte manifest was stored")
	}
	if _, err := s.Grant(x, id(1), "test"); err != nil {
		t.Fatal(err)
	}
	if gone, err := s.Delete(x); !gone || err != nil {
		t.Fatalf("delete: %v %v", gone, err)
	}
	if gone, _ := s.Delete(x); gone {
		t.Fatal("a second delete found an avatar")
	}
	if a, _ := s.Get(x); a != nil {
		t.Fatalf("after delete: %+v", a)
	}
	if items, _ := s.Items(x); len(items) != 1 {
		t.Fatalf("items after the avatar went: %v", items)
	}
}

func TestGrantsAndTheCatalog(t *testing.T) {
	s, clock := store(t)
	const x = 0x9000000000a01
	reward, free := id(1), id(2)
	s.Rewards = map[ItemID]bool{reward: true}
	m := make([]byte, ManifestSize)
	copy(m[FaceAt+FaceStep*3:], reward[:]) // a face texture slot
	copy(m[AssetAt+AssetStep*2:], free[:])
	var no *NotOwnedError
	if _, err := s.Save(x, m, "", []byte("png")); !errors.As(err, &no) || len(no.Items) != 1 || no.Items[0] != reward {
		t.Fatalf("an unowned reward item: %v", err)
	}
	if a, _ := s.Get(x); a != nil {
		t.Fatal("a refused save was stored")
	}
	if g, err := s.Grant(x, reward, "battlepass:s1:tier3"); !g || err != nil {
		t.Fatalf("grant: %v %v", g, err)
	}
	*clock += 10
	if g, err := s.Grant(x, reward, "test"); g || err != nil {
		t.Fatalf("a second grant: %v %v", g, err)
	}
	items, err := s.Items(x)
	if err != nil || len(items) != 1 || items[0].Item != reward || items[0].GrantedAt != 1800000000 || items[0].Source != "battlepass:s1:tier3" {
		t.Fatalf("items %+v %v", items, err)
	}
	if _, err := s.Save(x, m, "", []byte("png")); err != nil {
		t.Fatalf("owned now: %v", err)
	}
	var ref *RefusedError
	for _, c := range []struct {
		xuid   int64
		item   ItemID
		source string
	}{{0x9000000000a99, free, "test"}, {x, ItemID{}, "test"}, {x, free, ""}, {x, free, "two words"}, {x, free, strings.Repeat("s", 65)}} {
		if _, err := s.Grant(c.xuid, c.item, c.source); !errors.As(err, &ref) {
			t.Errorf("grant %#x %s %q: %v", c.xuid, c.item, c.source, err)
		}
	}
}
