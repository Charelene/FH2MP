// Package pyjson writes JSON the way Python's json.dumps does where the bytes matter (float repr, ensure_ascii, separators, insertion
// order via Obj).
package pyjson

import (
	"encoding/json"
	"fmt"
	"math"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"unicode/utf16"

	"fh2mp/internal/pyfmt"
)

// Pair is one key of an Obj.
type Pair struct {
	K string
	V any
}

// Obj is a JSON object that keeps insertion order (a Python dict).
type Obj []Pair

// Get returns the value of key k.
func (o Obj) Get(k string) (any, bool) {
	for _, p := range o {
		if p.K == k {
			return p.V, true
		}
	}
	return nil, false
}

// Set replaces or appends key k.
func (o *Obj) Set(k string, v any) {
	for i := range *o {
		if (*o)[i].K == k {
			(*o)[i].V = v
			return
		}
	}
	*o = append(*o, Pair{k, v})
}

// Opts are the dumps() options.
type Opts struct {
	Compact     bool // separators=(",", ":")
	NoASCII     bool // ensure_ascii=False
	SortKeys    bool
	DefaultStr  bool // default=str for values of unknown type
	ItemSep     string
	KeySep      string
	allowNaNOff bool
}

// Dumps is json.dumps(v, ...).
func Dumps(v any, o Opts) (string, error) {
	var b strings.Builder
	item, key := ", ", ": "
	if o.Compact {
		item, key = ",", ":"
	}
	if o.ItemSep != "" {
		item = o.ItemSep
	}
	if o.KeySep != "" {
		key = o.KeySep
	}
	o.ItemSep, o.KeySep = item, key
	if err := write(&b, reflect.ValueOf(v), o); err != nil {
		return "", err
	}
	return b.String(), nil
}

// Str is json.dumps of one string with the default options.
func Str(s string) string {
	var b strings.Builder
	quote(&b, s, false)
	return b.String()
}

func quote(b *strings.Builder, s string, noASCII bool) {
	b.WriteByte('"')
	for _, r := range s {
		switch r {
		case '"':
			b.WriteString(`\"`)
		case '\\':
			b.WriteString(`\\`)
		case '\n':
			b.WriteString(`\n`)
		case '\r':
			b.WriteString(`\r`)
		case '\t':
			b.WriteString(`\t`)
		case '\b':
			b.WriteString(`\b`)
		case '\f':
			b.WriteString(`\f`)
		default:
			switch {
			case r < 0x20:
				fmt.Fprintf(b, `\u%04x`, r)
			case r < 0x7f || (r >= 0x7f && noASCII):
				b.WriteRune(r)
			case r == 0x7f:
				b.WriteRune(r)
			case r > 0xffff:
				r1, r2 := utf16.EncodeRune(r)
				fmt.Fprintf(b, `\u%04x\u%04x`, r1, r2)
			default:
				fmt.Fprintf(b, `\u%04x`, r)
			}
		}
	}
	b.WriteByte('"')
}

func write(b *strings.Builder, v reflect.Value, o Opts) error {
	if !v.IsValid() {
		b.WriteString("null")
		return nil
	}
	if v.Kind() == reflect.Interface || v.Kind() == reflect.Pointer {
		if v.IsNil() {
			b.WriteString("null")
			return nil
		}
		return write(b, v.Elem(), o)
	}
	switch x := v.Interface().(type) {
	case Obj:
		keys := x
		if o.SortKeys {
			keys = append(Obj(nil), x...)
			sort.SliceStable(keys, func(i, j int) bool { return keys[i].K < keys[j].K })
		}
		b.WriteByte('{')
		for i, p := range keys {
			if i > 0 {
				b.WriteString(o.ItemSep)
			}
			quote(b, p.K, o.NoASCII)
			b.WriteString(o.KeySep)
			if err := write(b, reflect.ValueOf(p.V), o); err != nil {
				return err
			}
		}
		b.WriteByte('}')
		return nil
	}
	switch v.Kind() {
	case reflect.Bool:
		if v.Bool() {
			b.WriteString("true")
		} else {
			b.WriteString("false")
		}
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		b.WriteString(strconv.FormatInt(v.Int(), 10))
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		b.WriteString(strconv.FormatUint(v.Uint(), 10))
	case reflect.Float32, reflect.Float64:
		f := v.Float()
		switch {
		case math.IsNaN(f):
			b.WriteString("NaN")
		case math.IsInf(f, 1):
			b.WriteString("Infinity")
		case math.IsInf(f, -1):
			b.WriteString("-Infinity")
		default:
			b.WriteString(pyfmt.ReprFloat(f))
		}
	case reflect.String:
		quote(b, v.String(), o.NoASCII)
	case reflect.Slice, reflect.Array:
		if v.Kind() == reflect.Slice && v.Type().Elem().Kind() == reflect.Uint8 {
			quote(b, string(v.Bytes()), o.NoASCII)
			return nil
		}
		b.WriteByte('[')
		for i := 0; i < v.Len(); i++ {
			if i > 0 {
				b.WriteString(o.ItemSep)
			}
			if err := write(b, v.Index(i), o); err != nil {
				return err
			}
		}
		b.WriteByte(']')
	case reflect.Map:
		keys := make([]string, 0, v.Len())
		vals := map[string]reflect.Value{}
		it := v.MapRange()
		for it.Next() {
			k := fmt.Sprint(it.Key().Interface())
			keys = append(keys, k)
			vals[k] = it.Value()
		}
		sort.Strings(keys) // Go maps have no order: sorted, use Obj where Python's insertion order matters
		b.WriteByte('{')
		for i, k := range keys {
			if i > 0 {
				b.WriteString(o.ItemSep)
			}
			quote(b, k, o.NoASCII)
			b.WriteString(o.KeySep)
			if err := write(b, vals[k], o); err != nil {
				return err
			}
		}
		b.WriteByte('}')
	default:
		if o.DefaultStr {
			quote(b, fmt.Sprint(v.Interface()), o.NoASCII)
			return nil
		}
		return fmt.Errorf("pyjson: unsupported type %s", v.Type())
	}
	return nil
}

// Parse is json.loads: objects become Obj (keys in document order), arrays []any, integers int64 (uint64 when above int64), other numbers
// float64, then string, bool and nil.
func Parse(text string) (any, error) {
	dec := json.NewDecoder(strings.NewReader(text))
	dec.UseNumber()
	v, err := parseValue(dec)
	if err != nil {
		return nil, err
	}
	if dec.More() {
		return nil, fmt.Errorf("extra data")
	}
	if _, err := dec.Token(); err == nil {
		return nil, fmt.Errorf("extra data")
	}
	return v, nil
}

func parseValue(dec *json.Decoder) (any, error) {
	tok, err := dec.Token()
	if err != nil {
		return nil, err
	}
	switch t := tok.(type) {
	case json.Delim:
		switch t {
		case '{':
			obj := Obj{}
			for dec.More() {
				kt, err := dec.Token()
				if err != nil {
					return nil, err
				}
				v, err := parseValue(dec)
				if err != nil {
					return nil, err
				}
				obj.Set(kt.(string), v)
			}
			if _, err := dec.Token(); err != nil {
				return nil, err
			}
			return obj, nil
		case '[':
			arr := []any{}
			for dec.More() {
				v, err := parseValue(dec)
				if err != nil {
					return nil, err
				}
				arr = append(arr, v)
			}
			if _, err := dec.Token(); err != nil {
				return nil, err
			}
			return arr, nil
		}
	case json.Number:
		s := t.String()
		if !strings.ContainsAny(s, ".eE") {
			if i, err := strconv.ParseInt(s, 10, 64); err == nil {
				return i, nil
			}
			if u, err := strconv.ParseUint(s, 10, 64); err == nil {
				return u, nil
			}
		}
		f, err := t.Float64()
		return f, err
	case string, bool, nil:
		return t, nil
	}
	return nil, fmt.Errorf("unexpected token %v", tok)
}
