package pyjson

import "testing"

// The wording below is what python3.12 -c "import json; json.loads(...)" says.
func TestErrorText(t *testing.T) {
	for in, want := range map[string]string{
		"":                "Expecting value: line 1 column 1 (char 0)",
		`{"kind":`:        "Expecting value: line 1 column 9 (char 8)",
		`{`:               "Expecting property name enclosed in double quotes: line 1 column 2 (char 1)",
		`{"a" 1}`:         "Expecting ':' delimiter: line 1 column 6 (char 5)",
		`{"a":1 "b":2}`:   "Expecting ',' delimiter: line 1 column 8 (char 7)",
		`[1,2`:            "Expecting ',' delimiter: line 1 column 5 (char 4)",
		`{"a":1}x`:        "Extra data: line 1 column 8 (char 7)",
		`"abc`:            "Unterminated string starting at: line 1 column 1 (char 0)",
		"{\"a\":\n  nul}": "Expecting value: line 2 column 3 (char 8)",
		"[1,]":            "Expecting value: line 1 column 4 (char 3)",
		`{"a":1,}`:        "Expecting property name enclosed in double quotes: line 1 column 8 (char 7)",
		`"a\qb"`:          `Invalid \escape: line 1 column 3 (char 2)`,
		"\"a\tb\"":        "Invalid control character at: line 1 column 3 (char 2)",
		`{"a":-}`:         "Expecting value: line 1 column 6 (char 5)",
	} {
		if got := ErrorText(in); got != want {
			t.Errorf("%q: got %q, want %q", in, got, want)
		}
	}
	for _, ok := range []string{`{}`, `[1, 2.5e3, "x", null, true, false]`, `NaN`, ` {"a": {"b": []}} `} {
		if got := ErrorText(ok); got != "" {
			t.Errorf("%q: got %q, want no error", ok, got)
		}
	}
}
