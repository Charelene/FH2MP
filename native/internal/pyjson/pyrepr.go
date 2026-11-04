package pyjson

import (
	"strconv"
	"strings"

	"fh2mp/internal/pyfmt"
)

// TypeName is type(v).__name__ of a decoded JSON value.
func TypeName(v any) string {
	switch v.(type) {
	case nil:
		return "NoneType"
	case bool:
		return "bool"
	case int, int64, uint64:
		return "int"
	case float64:
		return "float"
	case string:
		return "str"
	case Obj:
		return "dict"
	case []any:
		return "list"
	}
	return "object"
}

// PyStr is str(v) of a decoded JSON value.
func PyStr(v any) string {
	if s, ok := v.(string); ok {
		return s
	}
	return PyRepr(v)
}

// PyRepr is repr(v) of a decoded JSON value.
func PyRepr(v any) string {
	switch x := v.(type) {
	case nil:
		return "None"
	case bool:
		if x {
			return "True"
		}
		return "False"
	case int:
		return strconv.Itoa(x)
	case int64:
		return strconv.FormatInt(x, 10)
	case uint64:
		return strconv.FormatUint(x, 10)
	case float64:
		return pyfmt.ReprFloat(x)
	case string:
		return pyfmt.ReprStr(x)
	case []any:
		parts := make([]string, len(x))
		for i, e := range x {
			parts[i] = PyRepr(e)
		}
		return "[" + strings.Join(parts, ", ") + "]"
	case Obj:
		parts := make([]string, len(x))
		for i, p := range x {
			parts[i] = pyfmt.ReprStr(p.K) + ": " + PyRepr(p.V)
		}
		return "{" + strings.Join(parts, ", ") + "}"
	}
	return "<object>"
}
