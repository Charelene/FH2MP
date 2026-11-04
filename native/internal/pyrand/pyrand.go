// Package pyrand is Python's random.Random (MT19937 seeded like random.seed(int)) with the functions the servers use, so that a seeded Go run draws
// the same numbers as a seeded Python run (the trace comparisons rely on it).
package pyrand

import (
	"crypto/sha512"
	"math"
	mb "math/bits"
)

const (
	n         = 624
	m         = 397
	matrixA   = 0x9908b0df
	upperMask = 0x80000000
	lowerMask = 0x7fffffff
)

// Random is an MT19937 generator.
type Random struct {
	mt  [n]uint32
	idx int
}

// New seeds like random.Random(seed) for a non-negative int seed.
func New(seed uint64) *Random {
	r := &Random{}
	r.Seed(seed)
	return r
}

func (r *Random) initGenrand(s uint32) {
	r.mt[0] = s
	for i := 1; i < n; i++ {
		r.mt[i] = 1812433253*(r.mt[i-1]^(r.mt[i-1]>>30)) + uint32(i)
	}
	r.idx = n
}

// Seed is random.seed(int): init_by_array with the 32-bit little-endian words of abs(seed).
func (r *Random) Seed(seed uint64) {
	key := []uint32{uint32(seed)}
	if seed>>32 != 0 {
		key = append(key, uint32(seed>>32))
	}
	r.seedKey(key)
}

// NewString seeds like random.Random("text"): the UTF-8 bytes followed by their SHA-512 digest, read as one big-endian integer.
func NewString(seed string) *Random {
	sum := sha512.Sum512([]byte(seed))
	b := append([]byte(seed), sum[:]...)
	var key []uint32
	for end := len(b); end > 0; end -= 4 {
		start := max(0, end-4)
		var w uint32
		for _, c := range b[start:end] {
			w = w<<8 | uint32(c)
		}
		key = append(key, w)
	}
	for len(key) > 1 && key[len(key)-1] == 0 {
		key = key[:len(key)-1]
	}
	r := &Random{}
	r.seedKey(key)
	return r
}

func (r *Random) seedKey(key []uint32) {
	r.initGenrand(19650218)
	i, j := 1, 0
	k := n
	if len(key) > k {
		k = len(key)
	}
	for ; k > 0; k-- {
		r.mt[i] = (r.mt[i] ^ ((r.mt[i-1] ^ (r.mt[i-1] >> 30)) * 1664525)) + key[j] + uint32(j)
		i++
		j++
		if i >= n {
			r.mt[0] = r.mt[n-1]
			i = 1
		}
		if j >= len(key) {
			j = 0
		}
	}
	for k = n - 1; k > 0; k-- {
		r.mt[i] = (r.mt[i] ^ ((r.mt[i-1] ^ (r.mt[i-1] >> 30)) * 1566083941)) - uint32(i)
		i++
		if i >= n {
			r.mt[0] = r.mt[n-1]
			i = 1
		}
	}
	r.mt[0] = 0x80000000
	r.idx = n
}

func (r *Random) genrand() uint32 {
	if r.idx >= n {
		var kk int
		mag := [2]uint32{0, matrixA}
		for kk = 0; kk < n-m; kk++ {
			y := (r.mt[kk] & upperMask) | (r.mt[kk+1] & lowerMask)
			r.mt[kk] = r.mt[kk+m] ^ (y >> 1) ^ mag[y&1]
		}
		for ; kk < n-1; kk++ {
			y := (r.mt[kk] & upperMask) | (r.mt[kk+1] & lowerMask)
			r.mt[kk] = r.mt[kk+(m-n)] ^ (y >> 1) ^ mag[y&1]
		}
		y := (r.mt[n-1] & upperMask) | (r.mt[0] & lowerMask)
		r.mt[n-1] = r.mt[m-1] ^ (y >> 1) ^ mag[y&1]
		r.idx = 0
	}
	y := r.mt[r.idx]
	r.idx++
	y ^= y >> 11
	y ^= (y << 7) & 0x9d2c5680
	y ^= (y << 15) & 0xefc60000
	y ^= y >> 18
	return y
}

// Float64 is random.random().
func (r *Random) Float64() float64 {
	a, b := r.genrand()>>5, r.genrand()>>6
	return (float64(a)*67108864.0 + float64(b)) * (1.0 / 9007199254740992.0)
}

// Getrandbits is random.getrandbits(k) for k <= 32.
func (r *Random) Getrandbits(k int) uint32 {
	if k <= 0 {
		return 0
	}
	return r.genrand() >> uint(32-k)
}

// Intn is random._randbelow(n): getrandbits(bit_length(n)) with rejection.
func (r *Random) Intn(nn int) int {
	if nn <= 0 {
		return 0
	}
	k := mb.Len(uint(nn))
	for {
		v := int(r.Getrandbits(k))
		if v < nn {
			return v
		}
	}
}

// Uniform is random.uniform(a, b).
func (r *Random) Uniform(a, b float64) float64 { return a + (b-a)*r.Float64() }

// Sample is random.sample(population, k) (CPython 3.12 algorithm).
func Sample[T any](r *Random, population []T, k int) []T {
	nn := len(population)
	if k < 0 || k > nn {
		panic("Sample larger than population or is negative")
	}
	result := make([]T, k)
	setsize := 21
	if k > 5 {
		setsize += int(math.Pow(4, math.Ceil(math.Log(float64(k*3))/math.Log(4))))
	}
	if nn <= setsize {
		pool := append([]T(nil), population...)
		for i := 0; i < k; i++ {
			j := r.Intn(nn - i)
			result[i] = pool[j]
			pool[j] = pool[nn-i-1]
		}
		return result
	}
	selected := map[int]bool{}
	for i := 0; i < k; i++ {
		j := r.Intn(nn)
		for selected[j] {
			j = r.Intn(nn)
		}
		selected[j] = true
		result[i] = population[j]
	}
	return result
}

// Shuffle is random.shuffle(x).
func Shuffle[T any](r *Random, x []T) {
	for i := len(x) - 1; i > 0; i-- {
		j := r.Intn(i + 1)
		x[i], x[j] = x[j], x[i]
	}
}
