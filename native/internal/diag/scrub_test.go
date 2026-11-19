package diag

import (
	"encoding/json"
	"math/rand"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The contract: Scrub gives exactly what the secrets regex gives, for every text; the shortcut only skips texts it cannot change.
func regexScrub(text string) string { return secrets.ReplaceAllString(text, "<redacted>") }

func TestScrubMatchesTheRegexOnCraftedTexts(t *testing.T) {
	for _, s := range []string{
		"", "plain line with nothing to hide", "token=abc", "TOKEN : abc", "the token is gone", "Bearer abc.def", "bearer\tx", "X-FH2MP-Token: fh2.abcdefgh",
		"x-fh2mp-token=1", "fh2.ABCDEFGHIJ rest", "fh2.short", "password=hunter2", "PassWord : x", "private_key = z", "Private Key: z", "privatekey=z",
		"secret=", "secret= ", "SECRET=s3", "toKen=kelvin", "ſecret=longs", "paſſword=x", "café token=1", "K only",
		"5001 *** CONNECTED *** (relay owner id 3; xuid 0x9000009bba248) run=ab12cd34.57",
	} {
		if got, want := Scrub(s), regexScrub(s); got != want {
			t.Errorf("%q: got %q, the regex gives %q", s, got, want)
		}
	}
}

// Every string in the recorded game traces (log lines and more) scrubs the same.
func TestScrubMatchesTheRegexOnTheRecordedTraces(t *testing.T) {
	files, _ := filepath.Glob("../../testdata/game_*.json")
	if len(files) == 0 {
		t.Skip("no traces")
	}
	n := 0
	var walk func(v any)
	walk = func(v any) {
		switch x := v.(type) {
		case string:
			n++
			if got, want := Scrub(x), regexScrub(x); got != want {
				t.Errorf("%q: got %q, the regex gives %q", x, got, want)
			}
		case []any:
			for _, e := range x {
				walk(e)
			}
		case map[string]any:
			for _, e := range x {
				walk(e)
			}
		}
	}
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		var v any
		if err := json.Unmarshal(b, &v); err != nil {
			t.Fatal(err)
		}
		walk(v)
	}
	if n < 1000 {
		t.Fatalf("only %d strings in the traces", n)
	}
}

// Random texts made of the letters the secret words use, separators, and now and then a folding character.
func TestScrubMatchesTheRegexOnRandomTexts(t *testing.T) {
	rng := rand.New(rand.NewSource(1))
	parts := []string{"t", "o", "k", "e", "n", "T", "O", "K", "E", "N", "s", "c", "r", "p", "a", "w", "d", "i", "v", "y", "b", "f", "h", "2", ".", "-",
		"_", " ", "=", ":", "\t", "x", "X", "M", "P", "K", "ſ", "Bearer ", "fh2.", "abcdefgh", "Token", "SECRET", "pass", "word", "private"}
	for i := 0; i < 20000; i++ {
		var b strings.Builder
		for k := rng.Intn(24); k >= 0; k-- {
			b.WriteString(parts[rng.Intn(len(parts))])
		}
		s := b.String()
		if got, want := Scrub(s), regexScrub(s); got != want {
			t.Fatalf("%q: got %q, the regex gives %q", s, got, want)
		}
	}
}

func BenchmarkScrubPlainLine(b *testing.B) {
	line := "5001 *** CONNECTED *** (relay owner id 3; xuid 0x9000009bba248, lobby type 1, handshake 254 ms, 6 other player(s) here) run=ab12cd34.57"
	for i := 0; i < b.N; i++ {
		Scrub(line)
	}
}
