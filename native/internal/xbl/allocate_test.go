package xbl

import (
	"testing"

	"fh2mp/internal/pyjson"
)

// sub walks nested objects of a session document: sub(doc, "servers", "gameServerPlatform") and so on.
func sub(o pyjson.Obj, keys ...string) any {
	var cur any = o
	for _, k := range keys {
		m, ok := cur.(pyjson.Obj)
		if !ok {
			return nil
		}
		cur, _ = m.Get(k)
	}
	return cur
}

// The private Free Roam the game creates itself (Nerd::CreatePrivateGame) allocates a server into its QoS session: a GSMS AllocateCluster that names
// the session, then polls that session for servers.gameServerPlatform. AllocateServer writes the block and moves the change number.
func TestAllocateServerWritesServersIntoQosSession(t *testing.T) {
	t.Setenv("FH2MP_PUBLIC_HOST", "203.0.113.7")
	s := New(&fakeHub{})
	path := "/serviceconfigs/788f0100-a4bc-46c3-b19b-d1001a96545b/sessionTemplates/defaultTemplate_qos/sessions/2a82bcdf-7021-49f5-add3-5c96642351d4"
	body := `{"constants":{"system":{"version":1,"visibility":"open"},"custom":{"hopperId":2,"EnableFullArbitration":false,` +
		`"gameServerPlatform":{"gameVariantId":"00000000-0000-0000-0000-000000000001","gsiSetId":"00000000-0000-0000-0000-000000000102","maxAllowedPlayers":12}}},` +
		`"members":{"me":{"constants":{"system":{"xuid":"2533277673447315","initialize":true}},` +
		`"properties":{"system":{"active":true,"secureDeviceAddress":"RkgyTVAtU0RBLTAwMDAwMDA="}}}}}`
	xuid := "2533277673447315"

	st, doc := s.PutSession(path, []byte(body), xuid)
	if st != 200 {
		t.Fatalf("PUT of the QoS session: status %d", st)
	}
	if got := sub(doc, "servers", "gameServerPlatform"); got != nil {
		t.Fatalf("a new session already lists a server: %v", got)
	}
	before, _ := doc.Get("changeNumber")

	if !s.AllocateServer(path) {
		t.Fatal("AllocateServer did not know the session it was just given")
	}
	st, doc = s.GetSession(path, xuid)
	if st != 200 {
		t.Fatalf("GET of the QoS session: status %d", st)
	}
	custom := sub(doc, "servers", "gameServerPlatform", "properties", "custom")
	c, ok := custom.(pyjson.Obj)
	if !ok {
		t.Fatalf("the session lists no allocated server: servers = %v", sub(doc, "servers"))
	}
	for k, want := range map[string]string{"status": "Active", "hostName": "203.0.113.7", "datacenter": "North Central US"} {
		if v, _ := c.Get(k); v != want {
			t.Errorf("servers...custom.%s = %v, want %q", k, v, want)
		}
	}
	after, _ := doc.Get("changeNumber")
	if toInt(after) <= toInt(before) {
		t.Errorf("the change number did not move: %v -> %v", before, after)
	}

	// A path that is no session path, and a session nobody created, are reported and change nothing.
	if s.AllocateServer("/not/a/session") {
		t.Error("a path that is no session path was accepted")
	}
	if s.AllocateServer("/serviceconfigs/788f0100-a4bc-46c3-b19b-d1001a96545b/sessionTemplates/defaultTemplate_qos/sessions/00000000-0000-4000-8000-000000000000") {
		t.Error("an unknown session was accepted")
	}
	// The same call a second time is harmless and keeps the block.
	if !s.AllocateServer(path) {
		t.Error("a second allocation into the same session was refused")
	}
}
