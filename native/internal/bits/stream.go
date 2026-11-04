package bits

import "errors"

// ErrTruncated is Python's ValueError("read past end").
var ErrTruncated = errors.New("read past end")

// Reader reads a datagram LSB first (= BitReader).
type Reader struct {
	data []byte
	Pos  int
	Size int
}

// NewReader reads data from bit 0.
func NewReader(data []byte) *Reader { return &Reader{data: data, Size: len(data) * 8} }

// Bits reads n <= 64 bits.
func (r *Reader) Bits(n int) (uint64, error) {
	if r.Pos+n > r.Size {
		return 0, ErrTruncated
	}
	var v uint64
	got := 0
	for got < n {
		byteIdx, bit := r.Pos>>3, uint(r.Pos&7)
		take := 8 - int(bit)
		if take > n-got {
			take = n - got
		}
		chunk := (uint64(r.data[byteIdx]) >> bit) & ((1 << uint(take)) - 1)
		v |= chunk << uint(got)
		got += take
		r.Pos += take
	}
	return v, nil
}

// Int reads n bits of any width.
func (r *Reader) Int(n int) (Int, error) {
	if r.Pos+n > r.Size {
		return nil, ErrTruncated
	}
	out := make([]uint64, (n+63)/64)
	for i := range out {
		take := 64
		if n-i*64 < 64 {
			take = n - i*64
		}
		v, _ := r.Bits(take)
		out[i] = v
	}
	return trim(out), nil
}

// Skip moves past n bits.
func (r *Reader) Skip(n int) error {
	if r.Pos+n > r.Size {
		return ErrTruncated
	}
	r.Pos += n
	return nil
}

// Varlen is cls:2 | value:(cls*8+6).
func (r *Reader) Varlen() (uint64, error) {
	cls, err := r.Bits(2)
	if err != nil {
		return 0, err
	}
	return r.Bits(int(cls)*8 + 6)
}

// String is varlen(len) | len*8 bits, latin-1.
func (r *Reader) String() (string, error) {
	n, err := r.Varlen()
	if err != nil {
		return "", err
	}
	if uint64(r.Size-r.Pos) < n*8 {
		return "", ErrTruncated
	}
	b := make([]rune, n)
	for i := range b {
		v, _ := r.Bits(8)
		b[i] = rune(v)
	}
	return string(b), nil
}

// Writer builds a bit string (= BitWriter). Words are little-endian.
type Writer struct {
	w   []uint64
	Pos int
}

func (w *Writer) grow(upto int) {
	for len(w.w)*64 < upto {
		w.w = append(w.w, 0)
	}
}

// Bits appends the low n <= 64 bits of v.
func (w *Writer) Bits(v uint64, n int) {
	if n == 0 {
		return
	}
	if n < 64 {
		v &= (1 << uint(n)) - 1
	}
	w.grow(w.Pos + n)
	wi, bi := w.Pos>>6, uint(w.Pos&63)
	w.w[wi] |= v << bi
	if bi != 0 && int(bi)+n > 64 {
		w.w[wi+1] |= v >> (64 - bi)
	}
	w.Pos += n
}

// Int appends the low n bits of v, any width.
func (w *Writer) Int(v Int, n int) {
	for off := 0; off < n; off += 64 {
		take := 64
		if n-off < 64 {
			take = n - off
		}
		w.Bits(v.Field(off, take), take)
	}
}

// Raw is Python's `value |= v << pos; pos += n`: all of v is OR-ed in at the current position, even bits past n, which then overlap what is
// written next (the Python code relies on payloads fitting their declared size, and so does this one; the spill is kept for byte parity).
func (w *Writer) Raw(v Int, n int) {
	if len(v) > 0 {
		w.grow(w.Pos + v.BitLen())
		ws, bs := w.Pos>>6, uint(w.Pos&63)
		for i, x := range v {
			w.w[ws+i] |= x << bs
			if bs != 0 && ws+i+1 < len(w.w) {
				w.w[ws+i+1] |= x >> (64 - bs)
			}
		}
	}
	w.Pos += n
}

// Varlen appends cls:2 | value.
func (w *Writer) Varlen(v uint64) {
	cls := 0
	switch {
	case v >= 1<<22:
		cls = 3
	case v >= 1<<14:
		cls = 2
	case v >= 1<<6:
		cls = 1
	}
	w.Bits(uint64(cls), 2)
	w.Bits(v, cls*8+6)
}

// String appends a latin-1 string; characters above U+00FF are not representable (Python raises).
func (w *Writer) String(s string) {
	raw := make([]byte, 0, len(s))
	for _, r := range s {
		raw = append(raw, byte(r))
	}
	w.Varlen(uint64(len(raw)))
	for _, b := range raw {
		w.Bits(uint64(b), 8)
	}
}

// Value is everything written so far as one Int.
func (w *Writer) Value() Int {
	out := make([]uint64, len(w.w))
	copy(out, w.w)
	return trim(out)
}

// Bytes is value.to_bytes((pos + 7) // 8, "little").
func (w *Writer) Bytes() []byte {
	n := (w.Pos + 7) / 8
	out := make([]byte, n)
	for i := range out {
		if i>>3 < len(w.w) {
			out[i] = byte(w.w[i>>3] >> (uint(i&7) * 8))
		}
	}
	return out
}
