package flags

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

// clock is a settable time, so the one-second re-check can be stepped over.
type clock struct{ t time.Time }

func (c *clock) now() time.Time { return c.t }

func newSet(t *testing.T, content string) (*Set, *clock, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "features.json")
	if content != "" {
		write(t, path, content, 1)
	}
	c := &clock{t: time.Unix(1000, 0)}
	s := New(path)
	s.Now = c.now
	return s, c, path
}

// write puts content in the file with a distinct modification time, so a rewrite of the same size is seen too.
func write(t *testing.T, path, content string, sec int64) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(path, time.Unix(sec, 0), time.Unix(sec, 0)); err != nil {
		t.Fatal(err)
	}
}

func TestEverythingIsOnWithoutAFile(t *testing.T) {
	for _, s := range []*Set{New(""), func() *Set { s, _, _ := newSet(t, ""); return s }()} {
		for _, k := range Known {
			if !s.On(k.Name) {
				t.Fatalf("%s is off without a file", k.Name)
			}
		}
		if off := s.Off(); len(off) != 0 {
			t.Fatalf("Off() = %v, want none", off)
		}
	}
	if !New("").On("no_such_feature") {
		t.Fatal("an unknown name must be on")
	}
}

func TestAFileSwitchesFeaturesOffAndTheRestStayOn(t *testing.T) {
	s, _, _ := newSet(t, `{"voice": false, "lobbies": true}`)
	if s.On(Voice) || !s.On(Lobbies) || !s.On(NOS) || !s.On(MTChat) {
		t.Fatalf("got %v, want only voice off", s.Snapshot())
	}
	if off := s.Off(); len(off) != 1 || off[0] != Voice {
		t.Fatalf("Off() = %v", off)
	}
}

func TestTheFileIsLookedAtOnceASecondAndFollowsEdits(t *testing.T) {
	s, c, path := newSet(t, `{"voice": false}`)
	if s.On(Voice) {
		t.Fatal("voice should be off")
	}
	write(t, path, `{"voice": true}`, 2)
	if s.On(Voice) {
		t.Fatal("the change was seen before the second was up")
	}
	c.t = c.t.Add(1100 * time.Millisecond)
	if !s.On(Voice) {
		t.Fatal("the change was not seen after a second")
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	write(t, path, `{"nos": false}`, 3)
	c.t = c.t.Add(2 * time.Second)
	if s.On(NOS) || !s.On(Voice) {
		t.Fatalf("got %v, want only nos off", s.Snapshot())
	}
	os.Remove(path)
	c.t = c.t.Add(2 * time.Second)
	if !s.On(NOS) {
		t.Fatal("deleting the file must put every feature back on")
	}
}

func TestABrokenFileKeepsTheLastGoodSwitches(t *testing.T) {
	s, c, path := newSet(t, `{"voice": false}`)
	if s.On(Voice) {
		t.Fatal("voice should be off")
	}
	write(t, path, `{"voice": tru`, 2)
	c.t = c.t.Add(2 * time.Second)
	if s.On(Voice) {
		t.Fatal("a half-written file switched voice back on")
	}
	write(t, path, `{"voice": true}`, 3)
	c.t = c.t.Add(2 * time.Second)
	if !s.On(Voice) {
		t.Fatal("the fixed file was not read")
	}
}

func TestATypoDoesNotStopTheRestOfTheFile(t *testing.T) {
	s, _, _ := newSet(t, `{"voice": false, "lobbys": false, "nos": "off"}`)
	if s.On(Voice) {
		t.Fatal("voice: false next to a typo must still switch voice off")
	}
	if !s.On(NOS) || !s.On(Lobbies) {
		t.Fatalf("a bad entry changed something: %v", s.Snapshot())
	}
	_, problems, err := Parse([]byte(`{"lobbys": false, "nos": "off"}`))
	if err != nil || len(problems) != 2 {
		t.Fatalf("problems = %v, err = %v, want the typo and the string named", problems, err)
	}
	if _, _, err := Parse([]byte(`[1]`)); err == nil {
		t.Fatal("a list is not a switches file")
	}
}

func TestUpdateWritesTheWholeSetAndAppliesAtOnce(t *testing.T) {
	s, _, path := newSet(t, `{"voice": false}`)
	if s.On(Voice) {
		t.Fatal("voice should be off")
	}
	if err := s.Update(map[string]bool{Lobbies: false}); err != nil {
		t.Fatal(err)
	}
	if s.On(Lobbies) || s.On(Voice) || !s.On(NOS) {
		t.Fatalf("got %v, want voice and lobbies off", s.Snapshot())
	}
	raw, _ := os.ReadFile(path)
	vals, problems, err := Parse(raw)
	if err != nil || len(problems) != 0 || vals[Voice] || vals[Lobbies] || !vals[NOS] || !vals[MTChat] {
		t.Fatalf("file %q parsed to %v (%v, %v)", raw, vals, problems, err)
	}
	if err := s.Update(map[string]bool{"nope": false}); err == nil {
		t.Fatal("an unknown feature was written")
	}
	if err := New("").Update(map[string]bool{Voice: false}); err == nil {
		t.Fatal("an update without a file path must fail")
	}
}
