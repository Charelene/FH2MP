// Package omap is an insertion-ordered map: Python's dict iteration order, which several behaviours of the servers depend on
// (deleting and re-adding a key moves it to the end).
package omap

// Map is an insertion-ordered map.
type Map[K comparable, V any] struct {
	idx  map[K]int
	keys []K
	vals []V
	live []bool
	dead int
}

// New returns an empty map.
func New[K comparable, V any]() *Map[K, V] { return &Map[K, V]{idx: map[K]int{}} }

// Len is the number of keys.
func (m *Map[K, V]) Len() int { return len(m.idx) }

// Get returns the value of k.
func (m *Map[K, V]) Get(k K) (V, bool) {
	if i, ok := m.idx[k]; ok {
		return m.vals[i], true
	}
	var zero V
	return zero, false
}

// Has reports whether k is present.
func (m *Map[K, V]) Has(k K) bool { _, ok := m.idx[k]; return ok }

// Set sets k; a new key goes to the end, an existing key keeps its place.
func (m *Map[K, V]) Set(k K, v V) {
	if i, ok := m.idx[k]; ok {
		m.vals[i] = v
		return
	}
	m.idx[k] = len(m.keys)
	m.keys = append(m.keys, k)
	m.vals = append(m.vals, v)
	m.live = append(m.live, true)
}

// Delete removes k and reports whether it was there.
func (m *Map[K, V]) Delete(k K) bool {
	i, ok := m.idx[k]
	if !ok {
		return false
	}
	delete(m.idx, k)
	var zk K
	var zv V
	m.keys[i], m.vals[i], m.live[i] = zk, zv, false
	m.dead++
	if m.dead > 32 && m.dead > len(m.keys)/2 {
		m.compact()
	}
	return true
}

func (m *Map[K, V]) compact() {
	n := 0
	for i := range m.keys {
		if m.live[i] {
			m.keys[n], m.vals[n], m.live[n] = m.keys[i], m.vals[i], true
			m.idx[m.keys[n]] = n
			n++
		}
	}
	var zk K
	var zv V
	for i := n; i < len(m.keys); i++ {
		m.keys[i], m.vals[i] = zk, zv
	}
	m.keys, m.vals, m.live = m.keys[:n], m.vals[:n], m.live[:n]
	m.dead = 0
}

// Keys returns a snapshot of the keys in order (Python's list(d)).
func (m *Map[K, V]) Keys() []K {
	out := make([]K, 0, len(m.idx))
	for i, k := range m.keys {
		if m.live[i] {
			out = append(out, k)
		}
	}
	return out
}

// Values returns a snapshot of the values in order.
func (m *Map[K, V]) Values() []V {
	out := make([]V, 0, len(m.idx))
	for i, v := range m.vals {
		if m.live[i] {
			out = append(out, v)
		}
	}
	return out
}

// Each calls fn for a snapshot of the entries in order; fn may change the map.
func (m *Map[K, V]) Each(fn func(K, V)) {
	for _, k := range m.Keys() {
		if v, ok := m.Get(k); ok {
			fn(k, v)
		}
	}
}

// First returns the first key.
func (m *Map[K, V]) First() (K, bool) {
	for i, k := range m.keys {
		if m.live[i] {
			return k, true
		}
	}
	var zero K
	return zero, false
}

// Clear removes everything.
func (m *Map[K, V]) Clear() { *m = *New[K, V]() }
