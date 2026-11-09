package lsp

import (
	"strings"
	"testing"
)

// The stand-in list the launcher sends: custom car ids with up to 5 stock candidates, up to 64 cars; anything else is refused before it
// reaches the game server's control socket.
func TestStandinsList(t *testing.T) {
	for _, ok := range []string{"9024:2164", "9024:2164/1215/1032,9025:1215", strings.TrimSuffix(strings.Repeat("1:2,", 64), ",")} {
		if !standinsOK(ok) {
			t.Errorf("refused %q", ok)
		}
	}
	for _, bad := range []string{"", "9024", "9024:", "9024:1/2/3/4/5/6", "a:1", "9024:2164 revoke 1", "9024:2164,", strings.TrimSuffix(strings.Repeat("1:2,", 65), ",")} {
		if standinsOK(bad) {
			t.Errorf("accepted %q", bad)
		}
	}
}
