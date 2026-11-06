// Package bits is the LSB-first bit stream the NetV2 protocol uses and the variable-width unsigned integer that carries
// message payloads (= Python's int in BitReader / BitWriter / bake, fh2mp_server.py:472-563).
package bits

import (
	"errors"
	mb "math/bits"
)

// Int is an unsigned integer of any width, little-endian 64-bit words with no trailing zero words. Operations return new values;
// a value is never modified after it is built, so it can be shared between peers' queues.
type Int []uint64

// FromUint64 returns x as an Int.
func FromUint64(x uint64) Int {
	if x == 0 {
		return nil
	}
	return Int{x}
}

// FromBytes reads b as a little-endian integer (int.from_bytes(b, "little")).
func FromBytes(b []byte) Int {
	w := make([]uint64, (len(b)+7)/8)
	for i, c := range b {
		w[i>>3] |= uint64(c) << (uint(i&7) * 8)
	}
	return trim(w)
}

func trim(w []uint64) Int {
	n := len(w)
	for n > 0 && w[n-1] == 0 {
		n--
	}
	if n == 0 {
		return nil
	}
	return Int(w[:n])
}

// ErrOverflow is Python's OverflowError from int.to_bytes.
var ErrOverflow = errors.New("int too big to convert")

// Bytes is x.to_bytes(n, "little").
func (x Int) Bytes(n int) ([]byte, error) {
	if x.BitLen() > n*8 {
		return nil, ErrOverflow
	}
	out := make([]byte, n)
	for i := range out {
		if i>>3 < len(x) {
			out[i] = byte(x[i>>3] >> (uint(i&7) * 8))
		}
	}
	return out, nil
}

// BitLen is int.bit_length().
func (x Int) BitLen() int {
	if len(x) == 0 {
		return 0
	}
	return (len(x)-1)*64 + mb.Len64(x[len(x)-1])
}

// IsZero reports x == 0.
func (x Int) IsZero() bool { return len(x) == 0 }

// Uint64 is x & 0xFFFFFFFFFFFFFFFF.
func (x Int) Uint64() uint64 {
	if len(x) == 0 {
		return 0
	}
	return x[0]
}

// Bit is (x >> i) & 1.
func (x Int) Bit(i int) uint64 {
	if i < 0 || i>>6 >= len(x) {
		return 0
	}
	return (x[i>>6] >> uint(i&63)) & 1
}

// Field is (x >> off) & ((1 << width) - 1) for width <= 64.
func (x Int) Field(off, width int) uint64 {
	if width == 0 {
		return 0
	}
	wi, bi := off>>6, uint(off&63)
	var v uint64
	if wi < len(x) {
		v = x[wi] >> bi
	}
	if bi != 0 && wi+1 < len(x) {
		v |= x[wi+1] << (64 - bi)
	}
	if width < 64 {
		v &= (1 << uint(width)) - 1
	}
	return v
}

// Lsh is x << n.
func (x Int) Lsh(n int) Int {
	if len(x) == 0 || n == 0 {
		return x
	}
	ws, bs := n>>6, uint(n&63)
	out := make([]uint64, len(x)+ws+1)
	for i, v := range x {
		out[i+ws] |= v << bs
		if bs != 0 {
			out[i+ws+1] |= v >> (64 - bs)
		}
	}
	return trim(out)
}

// Rsh is x >> n.
func (x Int) Rsh(n int) Int {
	ws, bs := n>>6, uint(n&63)
	if ws >= len(x) {
		return nil
	}
	out := make([]uint64, len(x)-ws)
	for i := range out {
		out[i] = x[i+ws] >> bs
		if bs != 0 && i+ws+1 < len(x) {
			out[i] |= x[i+ws+1] << (64 - bs)
		}
	}
	return trim(out)
}

// Low is x & ((1 << n) - 1).
func (x Int) Low(n int) Int {
	if n <= 0 {
		return nil
	}
	if n >= x.BitLen() {
		return x
	}
	out := make([]uint64, (n+63)/64)
	copy(out, x)
	if r := uint(n & 63); r != 0 {
		out[len(out)-1] &= (1 << r) - 1
	}
	return trim(out)
}

// Or is x | y.
func (x Int) Or(y Int) Int {
	if len(x) < len(y) {
		x, y = y, x
	}
	if len(y) == 0 {
		return x
	}
	out := make([]uint64, len(x))
	copy(out, x)
	for i, v := range y {
		out[i] |= v
	}
	return Int(out)
}

// And is x & y.
func (x Int) And(y Int) Int {
	n := len(x)
	if len(y) < n {
		n = len(y)
	}
	out := make([]uint64, n)
	for i := range out {
		out[i] = x[i] & y[i]
	}
	return trim(out)
}

// Equal reports x == y.
func (x Int) Equal(y Int) bool {
	if len(x) != len(y) {
		return false
	}
	for i := range x {
		if x[i] != y[i] {
			return false
		}
	}
	return true
}
