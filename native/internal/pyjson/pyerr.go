package pyjson

import (
	"fmt"
	"strings"
)

// ErrorText is what Python's json.loads says about text that is not JSON ("Expecting value: line 1 column 9 (char 8)"), or "" when Python would
// accept it. The scan follows json/decoder.py (3.12): it is only run after the real decoder has refused the text, to put Python's words on the refusal.
func ErrorText(text string) string {
	s := []rune(text)
	p := &scanner{s: s}
	p.skip()
	if _, ok := p.value(); !ok {
		return p.message()
	}
	p.skip()
	if p.i != len(s) {
		p.fail("Extra data", p.i)
		return p.message()
	}
	return ""
}

type scanner struct {
	s   []rune
	i   int
	msg string
	pos int
}

func (p *scanner) fail(msg string, pos int) bool { p.msg, p.pos = msg, pos; return false }

func (p *scanner) message() string {
	line, col := 1, p.pos+1
	last := -1
	for i := 0; i < p.pos && i < len(p.s); i++ {
		if p.s[i] == '\n' {
			line++
			last = i
		}
	}
	if last >= 0 {
		col = p.pos - last
	}
	return fmt.Sprintf("%s: line %d column %d (char %d)", p.msg, line, col, p.pos)
}

func (p *scanner) skip() {
	for p.i < len(p.s) && (p.s[p.i] == ' ' || p.s[p.i] == '\t' || p.s[p.i] == '\n' || p.s[p.i] == '\r') {
		p.i++
	}
}

func (p *scanner) peek() rune {
	if p.i < len(p.s) {
		return p.s[p.i]
	}
	return -1
}

func (p *scanner) lit(word string) bool {
	r := []rune(word)
	if p.i+len(r) <= len(p.s) && string(p.s[p.i:p.i+len(r)]) == word {
		p.i += len(r)
		return true
	}
	return false
}

// value is scan_once: the value at the cursor, or "Expecting value" at the cursor.
func (p *scanner) value() (struct{}, bool) {
	start := p.i
	switch c := p.peek(); {
	case c == '"':
		p.i++
		return struct{}{}, p.str(start)
	case c == '{':
		p.i++
		return struct{}{}, p.object()
	case c == '[':
		p.i++
		return struct{}{}, p.array()
	case c == 'n' && p.lit("null"), c == 't' && p.lit("true"), c == 'f' && p.lit("false"), c == 'N' && p.lit("NaN"), c == 'I' && p.lit("Infinity"):
		return struct{}{}, true
	case c == '-' && p.lit("-Infinity"):
		return struct{}{}, true
	}
	if p.number() {
		return struct{}{}, true
	}
	return struct{}{}, p.fail("Expecting value", start)
}

// number matches -?(0|[1-9]\d*)(\.\d+)?([eE][-+]?\d+)?
func (p *scanner) number() bool {
	i := p.i
	if i < len(p.s) && p.s[i] == '-' {
		i++
	}
	digits := func(j int) int {
		for j < len(p.s) && p.s[j] >= '0' && p.s[j] <= '9' {
			j++
		}
		return j
	}
	switch {
	case i < len(p.s) && p.s[i] == '0':
		i++
	case i < len(p.s) && p.s[i] >= '1' && p.s[i] <= '9':
		i = digits(i)
	default:
		return false
	}
	if i+1 < len(p.s) && p.s[i] == '.' && p.s[i+1] >= '0' && p.s[i+1] <= '9' {
		i = digits(i + 1)
	}
	if i < len(p.s) && (p.s[i] == 'e' || p.s[i] == 'E') {
		j := i + 1
		if j < len(p.s) && (p.s[j] == '-' || p.s[j] == '+') {
			j++
		}
		if j < len(p.s) && p.s[j] >= '0' && p.s[j] <= '9' {
			i = digits(j)
		}
	}
	p.i = i
	return true
}

// str is py_scanstring after the opening quote at begin.
func (p *scanner) str(begin int) bool {
	for {
		if p.i >= len(p.s) {
			return p.fail("Unterminated string starting at", begin)
		}
		c := p.s[p.i]
		switch {
		case c == '"':
			p.i++
			return true
		case c < 0x20:
			return p.fail("Invalid control character at", p.i)
		case c == '\\':
			p.i++
			if p.i >= len(p.s) {
				return p.fail("Unterminated string starting at", begin)
			}
			esc := p.s[p.i]
			if esc != 'u' {
				if !strings.ContainsRune(`"\/bfnrt`, esc) {
					return p.fail("Invalid \\escape", p.i-1)
				}
				p.i++
				continue
			}
			if p.i+4 >= len(p.s) || !isHex(p.s[p.i+1:p.i+5]) {
				return p.fail("Invalid \\uXXXX escape", p.i-1)
			}
			p.i += 5
		default:
			p.i++
		}
	}
}

func isHex(r []rune) bool {
	for _, c := range r {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F') {
			return false
		}
	}
	return true
}

func (p *scanner) object() bool {
	p.skip()
	switch c := p.peek(); {
	case c == '}':
		p.i++
		return true
	case c != '"':
		return p.fail("Expecting property name enclosed in double quotes", p.i)
	}
	for {
		begin := p.i
		p.i++
		if !p.str(begin) {
			return false
		}
		p.skip()
		if p.peek() != ':' {
			return p.fail("Expecting ':' delimiter", p.i)
		}
		p.i++
		p.skip()
		if _, ok := p.value(); !ok {
			return false
		}
		p.skip()
		c := p.peek()
		if c == '}' {
			p.i++
			return true
		}
		if c != ',' {
			return p.fail("Expecting ',' delimiter", p.i)
		}
		p.i++
		p.skip()
		if p.peek() != '"' {
			return p.fail("Expecting property name enclosed in double quotes", p.i)
		}
	}
}

func (p *scanner) array() bool {
	p.skip()
	if p.peek() == ']' {
		p.i++
		return true
	}
	for {
		if _, ok := p.value(); !ok {
			return false
		}
		p.skip()
		c := p.peek()
		if c == ']' {
			p.i++
			return true
		}
		if c != ',' {
			return p.fail("Expecting ',' delimiter", p.i)
		}
		p.i++
		p.skip()
	}
}
