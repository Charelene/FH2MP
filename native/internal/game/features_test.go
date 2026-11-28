package game

import (
	"os"
	"path/filepath"
	"testing"

	"fh2mp/internal/flags"
)

// switches points the servers' switches at a file with this content for the length of the test.
func switches(t *testing.T, content string) *flags.Set {
	t.Helper()
	path := filepath.Join(t.TempDir(), "features.json")
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	s := flags.New(path)
	t.Cleanup(flags.SetDefault(s))
	return s
}

// The operator's voice switch: off, a chat packet is read and dropped (nobody gets it); on again, the relay is as before, with no restart.
func TestVoiceIsNotRelayedWhileTheSwitchIsOff(t *testing.T) {
	set := switches(t, `{"voice": false}`)
	r, a, b, _ := voiceRig(t)
	r.w.Handle(clientVoice([]byte("VOICEPACKET-1-0123456789")), a.Addr)
	if n := len(sentTo(r, b.Addr)); n != 0 {
		t.Fatalf("voice switched off, but b got %d datagrams", n)
	}
	if err := set.Update(map[string]bool{flags.Voice: true}); err != nil {
		t.Fatal(err)
	}
	r.w.Handle(clientVoice([]byte("VOICEPACKET-2-0123456789")), a.Addr)
	if n := len(sentTo(r, b.Addr)); n != 1 {
		t.Fatalf("voice switched back on: b got %d datagrams, want 1", n)
	}
}

// The operator's nos switch: off, NitroSystem's NOS may burn nowhere, in the Free Roam world where it is allowed otherwise.
func TestNosIsNotAllowedWhileTheSwitchIsOff(t *testing.T) {
	set := switches(t, `{}`)
	r := newModsRig(t, 1, false)
	if !r.w.NosAllowed(r.peers[0]) {
		t.Fatal("NOS is not allowed in a Free Roam world with every switch on")
	}
	if err := set.Update(map[string]bool{flags.NOS: false}); err != nil {
		t.Fatal(err)
	}
	if r.w.NosAllowed(r.peers[0]) {
		t.Fatal("NOS allowed with the nos switch off")
	}
}
