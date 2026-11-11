package mt

import (
	"bufio"
	"fmt"
	"hash/crc32"
	"os"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/gotd/td/mt"
	"github.com/gotd/td/proto"

	"fh2mp/internal/mt/tl"
)

var (
	tlDefRe = regexp.MustCompile(`^([A-Za-z_][A-Za-z_.0-9]*)#([0-9a-f]{8})\b`)
	tlVecRe = regexp.MustCompile(`Vector<([^>]+)>`)
)

// canonicalTL is a definition as its CRC32 is taken: no id, no ';', Vector<X> as "Vector X", single spaces (Telegram's rule).
func canonicalTL(line string) string {
	s := strings.TrimSuffix(strings.TrimSpace(line), ";")
	s = tlDefRe.ReplaceAllString(s, "$1")
	s = tlVecRe.ReplaceAllString(s, "Vector $1")
	return strings.Join(strings.Fields(s), " ")
}

// TestSchemaIDs: every constructor id in proto/fh2mp.tl is the CRC32 of its definition, the generated code carries the same ids, and no id
// collides with an MTProto service constructor (both share one wire namespace inside an encrypted message).
func TestSchemaIDs(t *testing.T) {
	f, err := os.Open("../../../proto/fh2mp.tl")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	service := mt.TypesMap()
	for _, id := range []uint32{proto.MessageContainerTypeID, proto.GZIPTypeID, proto.ResultTypeID} {
		service[id] = fmt.Sprintf("proto %#08x", id)
	}
	generated := tl.TypesMap()
	seen := map[uint32]string{}
	n := 0
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		m := tlDefRe.FindStringSubmatch(line)
		if m == nil || m[1] == "vector" {
			continue
		}
		n++
		id64, _ := strconv.ParseUint(m[2], 16, 32)
		id := uint32(id64)
		if want := crc32.ChecksumIEEE([]byte(canonicalTL(line))); want != id {
			t.Errorf("%s: id %08x, CRC32 of %q is %08x", m[1], id, canonicalTL(line), want)
		}
		if prev, dup := seen[id]; dup {
			t.Errorf("%s and %s share id %08x", prev, m[1], id)
		}
		seen[id] = m[1]
		if name, clash := service[id]; clash && m[1] != "boolTrue" && m[1] != "boolFalse" && m[1] != "true" {
			t.Errorf("%s (%08x) collides with the MTProto service type %s", m[1], id, name)
		}
		if _, ok := generated[id]; !ok {
			t.Errorf("%s (%08x) is not in the generated code: run go generate ./internal/mt/tl", m[1], id)
		}
	}
	if n < 20 {
		t.Fatalf("only %d definitions read", n)
	}
	if len(generated) != len(seen) {
		t.Errorf("generated code has %d types, the schema %d (vector is built in): regenerate", len(generated), len(seen))
	}
}
