// Package pyfmt formats values the way Python does where the text reaches logs or wire bytes (repr of str and float, %g).
package pyfmt

import (
	"fmt"
	"math"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"
)

// ReprStr is repr(s) for a str.
func ReprStr(s string) string {
	quote := byte('\'')
	if strings.Contains(s, "'") && !strings.Contains(s, `"`) {
		quote = '"'
	}
	var b strings.Builder
	b.WriteByte(quote)
	for _, r := range s {
		switch {
		case r == rune(quote) || r == '\\':
			b.WriteByte('\\')
			b.WriteRune(r)
		case r == '\n':
			b.WriteString(`\n`)
		case r == '\r':
			b.WriteString(`\r`)
		case r == '\t':
			b.WriteString(`\t`)
		case r < 0x20 || r == 0x7f:
			fmt.Fprintf(&b, `\x%02x`, r)
		case r < 0x7f:
			b.WriteRune(r)
		case r == utf8.RuneError:
			b.WriteString(`�`)
		case unicode.IsPrint(r):
			b.WriteRune(r)
		case r <= 0xff:
			fmt.Fprintf(&b, `\x%02x`, r)
		case r <= 0xffff:
			fmt.Fprintf(&b, `\u%04x`, r)
		default:
			fmt.Fprintf(&b, `\U%08x`, r)
		}
	}
	b.WriteByte(quote)
	return b.String()
}

// ReprFloat is repr(x) for a float.
func ReprFloat(x float64) string {
	switch {
	case math.IsNaN(x):
		return "nan"
	case math.IsInf(x, 1):
		return "inf"
	case math.IsInf(x, -1):
		return "-inf"
	}
	if x == 0 {
		if math.Signbit(x) {
			return "-0.0"
		}
		return "0.0"
	}
	e := strconv.FormatFloat(x, 'e', -1, 64) // d.ddde+XX
	mant, exp, _ := strings.Cut(e, "e")
	ex, _ := strconv.Atoi(exp)
	if ex < -4 || ex >= 16 {
		sign := "+"
		if ex < 0 {
			sign = "-"
			ex = -ex
		}
		return fmt.Sprintf("%se%s%02d", mant, sign, ex)
	}
	s := strconv.FormatFloat(x, 'f', -1, 64)
	if !strings.ContainsAny(s, ".") {
		s += ".0"
	}
	return s
}

// FmtG is format(x, "g") / "%g" with the default 6 significant digits.
func FmtG(x float64) string { return FmtGPrec(x, 6) }

// FmtGPrec is "%.{prec}g".
func FmtGPrec(x float64, prec int) string {
	switch {
	case math.IsNaN(x):
		return "nan"
	case math.IsInf(x, 1):
		return "inf"
	case math.IsInf(x, -1):
		return "-inf"
	}
	if prec == 0 {
		prec = 1
	}
	if x == 0 {
		if math.Signbit(x) {
			return "-0"
		}
		return "0"
	}
	e := strconv.FormatFloat(x, 'e', prec-1, 64)
	mant, exp, _ := strings.Cut(e, "e")
	ex, _ := strconv.Atoi(exp)
	if ex < -4 || ex >= prec {
		if strings.Contains(mant, ".") {
			mant = strings.TrimRight(strings.TrimRight(mant, "0"), ".")
		}
		sign := "+"
		if ex < 0 {
			sign = "-"
			ex = -ex
		}
		return fmt.Sprintf("%se%s%02d", mant, sign, ex)
	}
	s := strconv.FormatFloat(x, 'f', prec-1-ex, 64)
	if strings.Contains(s, ".") {
		s = strings.TrimRight(strings.TrimRight(s, "0"), ".")
	}
	return s
}

// Fixed is format(x, ".Nf").
func Fixed(x float64, n int) string { return strconv.FormatFloat(x, 'f', n, 64) }

// F makes a float print like Python's format(): nan, inf and -inf in lower case, everything else as Go prints it.
type F float64

// Format implements fmt.Formatter.
func (x F) Format(s fmt.State, verb rune) {
	f := float64(x)
	var text string
	switch {
	case math.IsNaN(f):
		text = "nan"
	case math.IsInf(f, 1):
		text = "inf"
		if s.Flag('+') {
			text = "+inf"
		}
	case math.IsInf(f, -1):
		text = "-inf"
	default:
		spec := "%"
		for _, fl := range "+- 0#" {
			if s.Flag(int(fl)) {
				spec += string(fl)
			}
		}
		if w, ok := s.Width(); ok {
			spec += fmt.Sprint(w)
		}
		if p, ok := s.Precision(); ok {
			spec += "." + fmt.Sprint(p)
		}
		fmt.Fprintf(s, spec+string(verb), f)
		return
	}
	if w, ok := s.Width(); ok && len(text) < w {
		text = strings.Repeat(" ", w-len(text)) + text
	}
	fmt.Fprint(s, text)
}

// DecodeReplace is Python's bytes.decode("utf-8", "replace"): each maximal prefix of a well-formed sequence that is cut short or broken becomes one
// U+FFFD (Go's own conversion would give one per byte).
func DecodeReplace(b []byte) string {
	var out []byte
	for i := 0; i < len(b); {
		c := b[i]
		if c < 0x80 {
			out = append(out, c)
			i++
			continue
		}
		var lo, hi byte = 0x80, 0xBF
		need := 0
		switch {
		case c >= 0xC2 && c <= 0xDF:
			need = 1
		case c == 0xE0:
			need, lo = 2, 0xA0
		case c >= 0xE1 && c <= 0xEC, c == 0xEE, c == 0xEF:
			need = 2
		case c == 0xED:
			need, hi = 2, 0x9F
		case c == 0xF0:
			need, lo = 3, 0x90
		case c >= 0xF1 && c <= 0xF3:
			need = 3
		case c == 0xF4:
			need, hi = 3, 0x8F
		}
		if need == 0 {
			out = append(out, "\uFFFD"...)
			i++
			continue
		}
		j := i + 1
		ok := true
		for k := 0; k < need; k++ {
			if j >= len(b) || b[j] < lo || b[j] > hi {
				ok = false
				break
			}
			lo, hi = 0x80, 0xBF
			j++
		}
		if ok {
			out = append(out, b[i:j]...)
		} else {
			out = append(out, "\uFFFD"...)
		}
		i = j
	}
	return string(out)
}
