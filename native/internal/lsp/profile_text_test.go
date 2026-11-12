package lsp

import (
	"strings"
	"testing"

	"fh2mp/internal/auth"
)

func TestProfileTextIsTrimmedAndSpacesCollapsed(t *testing.T) {
	got, err := cleanProfileText(profileFields[0], "  S1   or nothing ", nil, nil)
	if err != nil || got != "S1 or nothing" {
		t.Fatalf("got %q, %v", got, err)
	}
}

func TestProfileTextEmptyClears(t *testing.T) {
	got, err := cleanProfileText(profileFields[3], "   ", nil, nil)
	if err != nil || got != "" {
		t.Fatalf("got %q, %v", got, err)
	}
}

func TestProfileTextLengthIsPerField(t *testing.T) {
	for _, f := range profileFields {
		if _, err := cleanProfileText(f, strings.Repeat("a", f.max), nil, nil); err != nil {
			t.Fatalf("%s of %d characters refused: %v", f.key, f.max, err)
		}
		_, err := cleanProfileText(f, strings.Repeat("a", f.max+1), nil, nil)
		if errCode(err) != "bad_profile_text" {
			t.Fatalf("%s of %d characters: %v", f.key, f.max+1, err)
		}
	}
	// characters, not bytes
	if _, err := cleanProfileText(profileFields[0], strings.Repeat("é", 21), nil, nil); err != nil {
		t.Fatalf("21 accented characters refused as a motto: %v", err)
	}
}

func TestProfileTextRefusesControlCharacters(t *testing.T) {
	if _, err := cleanProfileText(profileFields[3], "line one\nline two", nil, nil); errCode(err) != "bad_profile_text" {
		t.Fatalf("a newline passed: %v", err)
	}
}

func TestProfileTextRefusesBlockedFragmentsAndWords(t *testing.T) {
	blocked := []string{auth.FoldTag("badfrag")}
	words := []string{auth.FoldTag("ass")}
	_, err := cleanProfileText(profileFields[0], "my B a d-frag motto", blocked, nil)
	if errCode(err) != "bad_profile_text" || !strings.Contains(err.Error(), "isn't allowed") {
		t.Fatalf("a blocked fragment split by spaces passed: %v", err)
	}
	if _, err := cleanProfileText(profileFields[3], "what an ass.", nil, words); errCode(err) != "bad_profile_text" {
		t.Fatalf("a blocked word passed: %v", err)
	}
	// a blocked word inside another word is not that word
	if got, err := cleanProfileText(profileFields[3], "first class passes", nil, words); err != nil || got != "first class passes" {
		t.Fatalf("got %q, %v", got, err)
	}
}

// The Guide's themes: the default, Xbox, FH2MP, six colours and their plastic versions; nothing else.
func TestGuideThemesAreTheCanvasOnes(t *testing.T) {
	for _, ok := range []string{"", "xbox", "fh2mp", "red", "orange", "yellow", "green", "blue", "pink", "pink-plastic", "blue-plastic"} {
		if !GuideThemes[ok] {
			t.Errorf("theme %q refused", ok)
		}
	}
	for _, bad := range []string{"xbox-plastic", "fh2mp-plastic", "purple", "Red", " red", "red-plastic-plastic"} {
		if GuideThemes[bad] {
			t.Errorf("theme %q taken", bad)
		}
	}
}
