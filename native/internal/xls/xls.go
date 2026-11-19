// Package xls is the encoder/decoder of the Turn 10 "Xls" web-service structs (bin/xtsw) (= server/xls.py and the TableSource part of
// server/xls_schema.py). The schemas are read from the game catalog loaded into PostgreSQL (schema `game`, tables xls_field, xls_struct, xls_name).
package xls

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"sort"
	"unicode/utf8"

	"fh2mp/internal/db"
)

// Field is one entry of a struct descriptor.
type Field struct {
	VA, Type, Offset, Size, Nested, Elem, Flags, CountOff int64
	Name                                                  string
}

// Schema holds every struct descriptor, field entry and call name of the game.
type Schema struct {
	fields  map[int64]*Field
	structs map[int64][2]int64
	names   map[string][]int64
}

// LoadSchema reads the schema from the `game` schema of the database.
func LoadSchema(d *db.DB) (*Schema, error) {
	s := &Schema{fields: map[int64]*Field{}, structs: map[int64][2]int64{}, names: map[string][]int64{}}
	rows, err := d.Query(`SELECT va, type, name, "offset", size, nested, elem, flags, count_off FROM xls_field`)
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		s.fields[r.IntAt(0)] = &Field{VA: r.IntAt(0), Type: r.IntAt(1), Name: r.StrAt(2), Offset: r.IntAt(3), Size: r.IntAt(4), Nested: r.IntAt(5),
			Elem: r.IntAt(6), Flags: r.IntAt(7), CountOff: r.IntAt(8)}
	}
	rows, err = d.Query("SELECT desc_va, entries, count FROM xls_struct")
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		s.structs[r.IntAt(0)] = [2]int64{r.IntAt(1), r.IntAt(2)}
	}
	rows, err = d.Query("SELECT name, va FROM xls_name ORDER BY seq")
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		s.names[r.StrAt(0)] = append(s.names[r.StrAt(0)], r.IntAt(1))
	}
	return s, nil
}

// Count is the number of struct descriptors.
func (s *Schema) Count() int { return len(s.structs) }

// Names lists the call struct names, sorted.
func (s *Schema) Names() []string {
	out := make([]string, 0, len(s.names))
	for n := range s.names {
		out = append(out, n)
	}
	sort.Strings(out)
	return out
}

// Field returns the entry at a virtual address.
func (s *Schema) Field(va int64) *Field { return s.fields[va] }

// Fields lists the entries of a struct descriptor (nil when unknown).
func (s *Schema) Fields(desc int64) []*Field {
	e, ok := s.structs[desc]
	if !ok {
		return nil
	}
	out := make([]*Field, e[1])
	for i := range out {
		out[i] = s.fields[e[0]+0x60*int64(i)]
	}
	return out
}

// Sub is the field list of a nested struct field.
func (s *Schema) Sub(f *Field) []*Field { return s.Fields(f.Nested) }

// CallStruct is the root field list of e.g. "ForzaUserLogon_Results" (a single wrapper field holding the data).
func (s *Schema) CallStruct(name string) ([]*Field, error) {
	for _, ev := range s.names[name] {
		f := s.fields[ev]
		if f != nil && f.Type == 0xC {
			if fl := s.Fields(f.Nested); fl != nil {
				return fl, nil
			}
		}
	}
	return nil, fmt.Errorf("KeyError: %q", name)
}

// Struct is a decoded struct: its fields in schema order.
type Struct struct {
	Names []string
	Vals  map[string]any
}

// NewStruct makes an empty struct.
func NewStruct() *Struct { return &Struct{Vals: map[string]any{}} }

// Set sets a field (keeping the order of first assignment).
func (s *Struct) Set(k string, v any) {
	if _, ok := s.Vals[k]; !ok {
		s.Names = append(s.Names, k)
	}
	s.Vals[k] = v
}

// Get returns a field value (nil when absent).
func (s *Struct) Get(k string) any { return s.Vals[k] }

// Has reports whether the field exists.
func (s *Struct) Has(k string) bool { _, ok := s.Vals[k]; return ok }

// Int returns an integer field.
func (s *Struct) Int(k string) int64 { return AsInt(s.Vals[k]) }

// Str returns a string field.
func (s *Struct) Str(k string) string { v, _ := s.Vals[k].(string); return v }

// Bytes returns a blob field, or the 16 bytes of a GUID field (type 0x12 decodes to [16]byte). The Python codec gave bytes for both; reading
// only []byte made every Scoreboard.UploadFile scoreId and NotifyRival myScoreId empty, so no ghost was stored after the Go LSP went live.
func (s *Struct) Bytes(k string) []byte {
	switch v := s.Vals[k].(type) {
	case []byte:
		return v
	case [16]byte:
		return v[:]
	}
	return nil
}

// Float returns a float field.
func (s *Struct) Float(k string) float64 {
	switch v := s.Vals[k].(type) {
	case float64:
		return v
	case int64:
		return float64(v)
	}
	return 0
}

// Sub returns a nested struct field.
func (s *Struct) Sub(k string) *Struct { v, _ := s.Vals[k].(*Struct); return v }

// List returns an array field.
func (s *Struct) List(k string) []any { v, _ := s.Vals[k].([]any); return v }

// AsInt converts a decoded integer (int64 or uint64) to int64.
func AsInt(v any) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case uint64:
		return int64(x)
	case bool:
		if x {
			return 1
		}
	case float64:
		return int64(x)
	}
	return 0
}

// Clone copies a struct deeply.
func (s *Struct) Clone() *Struct {
	out := NewStruct()
	for _, k := range s.Names {
		out.Set(k, cloneValue(s.Vals[k]))
	}
	return out
}

func cloneValue(v any) any {
	switch x := v.(type) {
	case *Struct:
		return x.Clone()
	case []any:
		out := make([]any, len(x))
		for i, e := range x {
			out[i] = cloneValue(e)
		}
		return out
	case []byte:
		return append([]byte(nil), x...)
	}
	return v
}

// ErrShortRead is Python's ValueError("short read").
var ErrShortRead = errors.New("short read")

type reader struct {
	d []byte
	p int
}

func (r *reader) take(n int) ([]byte, error) {
	if n < 0 || r.p+n > len(r.d) {
		return nil, ErrShortRead
	}
	b := r.d[r.p : r.p+n]
	r.p += n
	return b, nil
}

func (s *Schema) readValue(r *reader, f *Field) (any, error) {
	switch f.Type {
	case 0:
		b, err := r.take(1)
		if err != nil {
			return nil, err
		}
		return int64(b[0]), nil
	case 1:
		b, err := r.take(2)
		if err != nil {
			return nil, err
		}
		return int64(binary.BigEndian.Uint16(b)), nil
	case 2:
		b, err := r.take(2)
		if err != nil {
			return nil, err
		}
		return int64(int16(binary.BigEndian.Uint16(b))), nil
	case 3:
		b, err := r.take(4)
		if err != nil {
			return nil, err
		}
		return int64(int32(binary.BigEndian.Uint32(b))), nil
	case 4:
		b, err := r.take(4)
		if err != nil {
			return nil, err
		}
		return int64(binary.BigEndian.Uint32(b)), nil
	case 5:
		b, err := r.take(8)
		if err != nil {
			return nil, err
		}
		return int64(binary.BigEndian.Uint64(b)), nil
	case 6, 0x13:
		b, err := r.take(8)
		if err != nil {
			return nil, err
		}
		return binary.BigEndian.Uint64(b), nil
	case 7:
		b, err := r.take(4)
		if err != nil {
			return nil, err
		}
		return float64(math.Float32frombits(binary.BigEndian.Uint32(b))), nil
	case 0x11:
		b, err := r.take(1)
		if err != nil {
			return nil, err
		}
		return b[0] != 0, nil
	case 9, 0x10:
		b, err := r.take(4)
		if err != nil {
			return nil, err
		}
		data, err := r.take(int(binary.BigEndian.Uint32(b)))
		if err != nil {
			return nil, err
		}
		return decodeUTF8Replace(data), nil
	case 0x14:
		b, err := r.take(4)
		if err != nil {
			return nil, err
		}
		data, err := r.take(int(binary.BigEndian.Uint32(b)))
		if err != nil {
			return nil, err
		}
		return append([]byte(nil), data...), nil
	case 0x12:
		b, err := r.take(16)
		if err != nil {
			return nil, err
		}
		var g [16]byte
		copy(g[:], b)
		return g, nil
	case 0xC:
		return s.readStruct(r, s.Sub(f))
	case 0xE:
		b, err := r.take(4)
		if err != nil {
			return nil, err
		}
		n := int(binary.BigEndian.Uint32(b))
		el := s.fields[f.Elem]
		var out []any
		for i := 0; i < n; i++ {
			v, err := s.readValue(r, el)
			if err != nil {
				return nil, err
			}
			out = append(out, v)
		}
		if out == nil {
			out = []any{}
		}
		return out, nil
	}
	return nil, fmt.Errorf("unsupported type %#x for %s", f.Type, f.Name)
}

// decodeUTF8Replace is bytes.decode("utf-8", "replace").
func decodeUTF8Replace(b []byte) string {
	if utf8.Valid(b) {
		return string(b)
	}
	return string([]rune(string(b)))
}

func (s *Schema) readStruct(r *reader, fields []*Field) (*Struct, error) {
	out := NewStruct()
	for _, f := range fields {
		v, err := s.readValue(r, f)
		if err != nil {
			return nil, err
		}
		out.Set(f.Name, v)
	}
	return out, nil
}

// Decode reads a struct from data and returns the number of bytes left over.
func (s *Schema) Decode(fields []*Field, data []byte) (*Struct, int, error) {
	r := &reader{d: data}
	v, err := s.readStruct(r, fields)
	if err != nil {
		return nil, 0, err
	}
	return v, len(data) - r.p, nil
}

func put(out *[]byte, bs ...byte) { *out = append(*out, bs...) }

func be(out *[]byte, v uint64, n int) {
	for i := n - 1; i >= 0; i-- {
		*out = append(*out, byte(v>>(8*uint(i))))
	}
}

// fixedRange checks that v fits the wire type like struct.pack does (it raises struct.error otherwise).
func fixedRange(f *Field, v any) (uint64, error) {
	var i int64
	var u uint64
	isU := false
	switch x := v.(type) {
	case int64:
		i = x
	case int:
		i = int64(x)
	case uint64:
		u, isU = x, true
	case bool:
		if x {
			i = 1
		}
	default:
		return 0, fmt.Errorf("argument for '%s' must be an integer, not %T", f.Name, v)
	}
	bad := func() (uint64, error) { return 0, fmt.Errorf("struct.error: %s: value out of range", f.Name) }
	switch f.Type {
	case 0:
		if isU || i < 0 || i > 0xFF {
			return bad()
		}
	case 1:
		if isU || i < 0 || i > 0xFFFF {
			return bad()
		}
	case 3:
		if isU || i < math.MinInt32 || i > math.MaxInt32 {
			return bad()
		}
	case 4:
		if isU || i < 0 || i > 0xFFFFFFFF {
			return bad()
		}
	case 5:
		if isU && u > math.MaxInt64 {
			return bad()
		}
	case 6, 0x13:
		if !isU && i < 0 {
			return bad()
		}
	}
	if isU {
		return u, nil
	}
	return uint64(i), nil
}

func (s *Schema) writeValue(out *[]byte, f *Field, v any) error {
	switch f.Type {
	case 2: // the same 16 bits on the wire; notificationType 0x8502 etc. exceed the signed range
		x, ok := toInt(v)
		if !ok {
			return fmt.Errorf("%s: not an integer", f.Name)
		}
		be(out, uint64(x)&0xFFFF, 2)
	case 0, 1, 3, 4, 5, 6, 0x13:
		n := map[int64]int{0: 1, 1: 2, 3: 4, 4: 4, 5: 8, 6: 8, 0x13: 8}[f.Type]
		u, err := fixedRange(f, v)
		if err != nil {
			return err
		}
		be(out, u, n)
	case 7:
		fl, ok := v.(float64)
		if !ok {
			if i, ok2 := toInt(v); ok2 {
				fl = float64(i)
			} else {
				return fmt.Errorf("%s: not a float", f.Name)
			}
		}
		be(out, uint64(math.Float32bits(float32(fl))), 4)
	case 0x11:
		b, _ := v.(bool)
		if i, ok := toInt(v); ok {
			b = i != 0
		}
		if b {
			put(out, 1)
		} else {
			put(out, 0)
		}
	case 9, 0x10:
		str, ok := v.(string)
		if !ok {
			return fmt.Errorf("%s: not a string", f.Name)
		}
		be(out, uint64(len(str)), 4)
		*out = append(*out, str...)
	case 0x14:
		var b []byte
		switch x := v.(type) {
		case []byte:
			b = x
		case string:
			b = []byte(x)
		default:
			return fmt.Errorf("%s: not bytes", f.Name)
		}
		be(out, uint64(len(b)), 4)
		*out = append(*out, b...)
	case 0x12:
		switch x := v.(type) {
		case [16]byte:
			*out = append(*out, x[:]...)
		case []byte:
			*out = append(*out, x...)
		default:
			return fmt.Errorf("%s: not a guid", f.Name)
		}
	case 0xC:
		st, ok := v.(*Struct)
		if !ok {
			return fmt.Errorf("%s: not a struct", f.Name)
		}
		return s.writeStruct(out, s.Sub(f), st)
	case 0xE:
		list, ok := v.([]any)
		if !ok {
			return fmt.Errorf("%s: not a list", f.Name)
		}
		el := s.fields[f.Elem]
		if f.Size != 0 && el.Size != 0 && int64(len(list)) > f.Size/el.Size {
			return fmt.Errorf("%s: %d elements exceeds the client's capacity %d", f.Name, len(list), f.Size/el.Size)
		}
		be(out, uint64(len(list)), 4)
		for _, x := range list {
			if err := s.writeValue(out, el, x); err != nil {
				return err
			}
		}
	default:
		return fmt.Errorf("unsupported type %#x for %s", f.Type, f.Name)
	}
	return nil
}

func toInt(v any) (int64, bool) {
	switch x := v.(type) {
	case int64:
		return x, true
	case int:
		return int64(x), true
	case uint64:
		return int64(x), true
	}
	return 0, false
}

func (s *Schema) writeStruct(out *[]byte, fields []*Field, v *Struct) error {
	for _, f := range fields {
		val, ok := v.Vals[f.Name]
		if !ok {
			return fmt.Errorf("KeyError: %q", f.Name)
		}
		if err := s.writeValue(out, f, val); err != nil {
			return err
		}
	}
	return nil
}

// Encode writes a struct.
func (s *Schema) Encode(fields []*Field, v *Struct) ([]byte, error) {
	var out []byte
	if err := s.writeStruct(&out, fields, v); err != nil {
		return nil, err
	}
	return out, nil
}

// DefaultStruct is a struct with every field at its default value.
func (s *Schema) DefaultStruct(fields []*Field) (*Struct, error) {
	out := NewStruct()
	for _, f := range fields {
		v, err := s.defaultValue(f)
		if err != nil {
			return nil, err
		}
		out.Set(f.Name, v)
	}
	return out, nil
}

func (s *Schema) defaultValue(f *Field) (any, error) {
	switch f.Type {
	case 0, 1, 2, 3, 4, 5:
		return int64(0), nil
	case 6, 0x13:
		return uint64(0), nil
	case 7:
		return 0.0, nil
	case 0x11:
		return false, nil
	case 9:
		return "", nil
	case 0x10:
		return "1970-01-01T00:00:00Z", nil
	case 0x14:
		return []byte{}, nil
	case 0x12:
		return [16]byte{}, nil
	case 0xC:
		return s.DefaultStruct(s.Sub(f))
	case 0xE:
		return []any{}, nil
	}
	return nil, fmt.Errorf("unsupported type %#x", f.Type)
}
