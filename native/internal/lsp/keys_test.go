package lsp

import (
	"os"
	"sort"
	"strings"
	"testing"
)

// TestHandlerKeys: every call the Python server answers has a handler here (and the other way round). The list is made by reading the HANDLERS table of
// server/fh2mp_lsp.py (tools/golden/vectors_lsp_keys.py).
func TestHandlerKeys(t *testing.T) {
	raw, err := os.ReadFile("../../testdata/lsp_handler_keys.txt")
	if err != nil {
		t.Skip("no key list: ", err)
	}
	want := map[string]bool{}
	for _, l := range strings.Split(strings.TrimSpace(string(raw)), "\n") {
		want[l] = true
	}
	have := map[string]bool{}
	for k := range handlers {
		have[k[0]+"|"+k[1]] = true
	}
	var missing, extra []string
	for k := range want {
		if !have[k] {
			missing = append(missing, k)
		}
	}
	for k := range have {
		if !want[k] {
			extra = append(extra, k)
		}
	}
	sort.Strings(missing)
	sort.Strings(extra)
	if len(missing)+len(extra) > 0 {
		t.Errorf("missing %v\nextra %v", missing, extra)
	}
}
