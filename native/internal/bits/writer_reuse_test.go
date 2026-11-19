package bits

import (
	"bytes"
	"math/rand"
	"testing"
)

// Reserve only makes room and AppendBytes only skips a copy: for random writes (Bits, Raw with its spill, Int) both give the bytes the plain
// writer gives.
func TestReserveAndAppendBytesChangeNothing(t *testing.T) {
	rng := rand.New(rand.NewSource(7))
	for round := 0; round < 2000; round++ {
		var plain, reserved Writer
		reserved.Reserve(rng.Intn(2000))
		for k := rng.Intn(12); k >= 0; k-- {
			n := 1 + rng.Intn(64)
			v := rng.Uint64()
			switch rng.Intn(3) {
			case 0:
				plain.Bits(v, n)
				reserved.Bits(v, n)
			case 1:
				x := FromUint64(v).Lsh(rng.Intn(200))
				size := x.BitLen() - rng.Intn(3) // Raw lets a value spill past its declared size
				if size < 0 {
					size = 0
				}
				plain.Raw(x, size)
				reserved.Raw(x, size)
			default:
				x := FromUint64(v).Lsh(rng.Intn(100)).Or(FromUint64(rng.Uint64()))
				plain.Int(x, 1+rng.Intn(160))
				reserved.Int(x, plain.Pos-reserved.Pos+0)
			}
		}
		prefix := []byte{1, 2, 3}
		want := append(append([]byte(nil), prefix...), plain.Bytes()...)
		if got := reserved.AppendBytes(append([]byte(nil), prefix...)); !bytes.Equal(got, want) || reserved.Pos != plain.Pos {
			t.Fatalf("round %d: got % x, want % x", round, got, want)
		}
	}
}
