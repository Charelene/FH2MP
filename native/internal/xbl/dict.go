package xbl

import (
	"fh2mp/internal/pyjson"
)

// Dict is an insertion-ordered JSON object with reference semantics (a Python dict): edits anywhere in a tree are seen by every holder.
type Dict struct {
	keys []string
	m    map[string]any
}

// NewDict makes an empty Dict.
func NewDict() *Dict { return &Dict{m: map[string]any{}} }

// Get returns the value of k.
func (d *Dict) Get(k string) (any, bool) {
	v, ok := d.m[k]
	return v, ok
}

// Has reports whether k is set.
func (d *Dict) Has(k string) bool { _, ok := d.m[k]; return ok }

// Set stores v under k (position kept when the key exists).
func (d *Dict) Set(k string, v any) {
	if _, ok := d.m[k]; !ok {
		d.keys = append(d.keys, k)
	}
	d.m[k] = v
}

// Delete removes k and returns its value.
func (d *Dict) Delete(k string) (any, bool) {
	v, ok := d.m[k]
	if !ok {
		return nil, false
	}
	delete(d.m, k)
	for i, x := range d.keys {
		if x == k {
			d.keys = append(d.keys[:i:i], d.keys[i+1:]...)
			break
		}
	}
	return v, true
}

// Keys lists the keys in insertion order.
func (d *Dict) Keys() []string { return append([]string(nil), d.keys...) }

// Len is the number of keys.
func (d *Dict) Len() int { return len(d.keys) }

// Sub returns the nested Dict under k, creating it when absent (setdefault(k, {})). ok=false when k holds something that is not an object.
func (d *Dict) Sub(k string) (*Dict, bool) {
	v, has := d.m[k]
	if !has {
		n := NewDict()
		d.Set(k, n)
		return n, true
	}
	n, ok := v.(*Dict)
	return n, ok
}

// SubOrNil returns the nested Dict under k, or nil when it is absent or not an object (get(k) or {}, read-only).
func (d *Dict) SubOrNil(k string) *Dict {
	if d == nil {
		return nil
	}
	n, _ := d.m[k].(*Dict)
	return n
}

// FromJSON converts a decoded JSON tree (pyjson.Obj, []any, scalars) to Dict trees.
func FromJSON(v any) any {
	switch x := v.(type) {
	case pyjson.Obj:
		d := NewDict()
		for _, p := range x {
			d.Set(p.K, FromJSON(p.V))
		}
		return d
	case []any:
		out := make([]any, len(x))
		for i, e := range x {
			out[i] = FromJSON(e)
		}
		return out
	}
	return v
}

// ToJSON converts Dict trees to pyjson.Obj trees for serialising.
func ToJSON(v any) any {
	switch x := v.(type) {
	case *Dict:
		if x == nil {
			return nil
		}
		o := make(pyjson.Obj, 0, len(x.keys))
		for _, k := range x.keys {
			o = append(o, pyjson.Pair{K: k, V: ToJSON(x.m[k])})
		}
		return o
	case []any:
		out := make([]any, len(x))
		for i, e := range x {
			out[i] = ToJSON(e)
		}
		return out
	}
	return v
}

// DeepCopy is copy.deepcopy.
func DeepCopy(v any) any {
	switch x := v.(type) {
	case *Dict:
		if x == nil {
			return (*Dict)(nil)
		}
		n := NewDict()
		for _, k := range x.keys {
			n.Set(k, DeepCopy(x.m[k]))
		}
		return n
	case []any:
		out := make([]any, len(x))
		for i, e := range x {
			out[i] = DeepCopy(e)
		}
		return out
	}
	return v
}

func (d *Dict) copyDict() *Dict { return DeepCopy(d).(*Dict) }

// Merge is the recursive dict merge of an MPSD PUT (a partial update).
func Merge(dst, src *Dict) *Dict {
	for _, k := range src.keys {
		v := src.m[k]
		if sv, ok := v.(*Dict); ok {
			if dv, ok := dst.m[k].(*Dict); ok {
				Merge(dv, sv)
				continue
			}
		}
		dst.Set(k, DeepCopy(v))
	}
	return dst
}

// TypeName is type(v).__name__ for a tree value.
func TypeName(v any) string {
	if _, ok := v.(*Dict); ok {
		return "dict"
	}
	return pyjson.TypeName(v)
}

// Repr is repr(v) for a tree value.
func Repr(v any) string { return pyjson.PyRepr(ToJSON(v)) }
