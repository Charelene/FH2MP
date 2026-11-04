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
