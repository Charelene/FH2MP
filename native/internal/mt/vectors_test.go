package mt

import (
	"bytes"
	"crypto/aes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"os"
	"testing"

	"github.com/gotd/ige"
	"github.com/gotd/td/bin"
	"github.com/gotd/td/crypto"

	"fh2mp/internal/auth"
	"fh2mp/internal/mt/tl"
)

// The cross-language vectors of the chat protocol (native/testdata/mt_vectors.json), read by this test and by the Rust client
// (client/crates/fh2mp-mtproto/tests/vectors.rs). Regenerate with `go test ./internal/mt -run TestVectors -update` only when the protocol
// changes on purpose; both sides must then agree on the new bytes.

var updateVectors = flag.Bool("update", false, "rewrite native/testdata/mt_vectors.json")

const vectorPath = "../../testdata/mt_vectors.json"

type signVector struct {
	Realm     string   `json:"realm"`
	Purpose   string   `json:"purpose"`
	Challenge string   `json:"challenge"`
	Fields    []string `json:"fields"`
	Message   string   `json:"message"`
}

type tlVector struct {
	Name string `json:"name"`
	Hex  string `json:"hex"`
}

type cryptoVector struct {
	AuthKey        string `json:"auth_key"`
	KeyID          string `json:"key_id"`
	Plaintext      string `json:"plaintext"`
	ClientToServer string `json:"client_to_server"`
	ServerToClient string `json:"server_to_client"`
}

type vectorFile struct {
	Note   string       `json:"note"`
	Sign   []signVector `json:"sign_message"`
	TL     []tlVector   `json:"tl"`
	Crypto cryptoVector `json:"crypto"`
}

func det(n int, seed string) []byte {
	var out []byte
	for i := 0; len(out) < n; i++ {
		h := sha256.Sum256([]byte(seed + string(rune('a'+i%26)) + string(rune(i))))
		out = append(out, h[:]...)
	}
	return out[:n]
}

func makeVectors(t *testing.T) vectorFile {
	h := hex.EncodeToString
	chal := det(41, "challenge")
	pub := det(32, "pubkey")
	keyID := det(8, "keyid")
	dev := []byte{0x2a, 0, 0, 0, 0, 0, 0, 0}
	v := vectorFile{Note: "frozen; regenerate with go test ./internal/mt -run TestVectors -update (protocol change only)"}
	v.Sign = append(v.Sign, signVector{Realm: "prod", Purpose: Purpose, Challenge: h(chal), Fields: []string{h(pub), h(keyID), h(dev)},
		Message: h(auth.SignMessage("prod", Purpose, chal, pub, keyID, dev))})
	objs := []struct {
		name string
		o    bin.Encoder
	}{
		{"help.init", &tl.HelpInitRequest{Layer: 1, AppVersion: "0.1.0", Platform: "windows"}},
		{"help.config", &tl.HelpConfig{Layer: 1, Date: 1_800_000_000, MaxRequestBytes: 1047552, KeyTTL: 86399}},
		{"auth.challenge", &tl.AuthChallenge{Challenge: chal, Realm: "prod", ExpiresIn: 120}},
		{"auth.signIn", &tl.AuthSignInRequest{Challenge: chal, Pubkey: pub, Signature: det(64, "sig"), DeviceID: -7}},
		{"auth.authorization", &tl.AuthAuthorization{User: tl.User{ID: 0x9000012345678, Gamertag: "Maplefox21378"}}},
		{"updates.getDifference", &tl.UpdatesGetDifferenceRequest{Pts: 41, Limit: 100}},
		{"updates", &tl.Updates{Updates: []tl.UpdateClass{&tl.UpdateNotice{ID: 99, Text: "hello ✓", Date: 1_800_000_001},
			&tl.UpdateNotice{ID: -1, Text: "", Date: 0}}, Pts: 43, PtsCount: 2, Date: 1_800_000_002}},
		{"updateShort", &tl.UpdateShort{Update: &tl.UpdateLoggedOut{Reason: "signed in on another device"}, Date: 1_800_000_003}},
		{"updates.differenceSlice", &tl.UpdatesDifferenceSlice{NewUpdates: []tl.UpdateClass{&tl.UpdateNotice{ID: 5, Text: "x", Date: 6}},
			IntermediateState: tl.UpdatesState{Pts: 42, Date: 1_800_000_004}}},
		{"updates.differenceTooLong", &tl.UpdatesDifferenceTooLong{Pts: 7}},
	}
	for _, o := range objs {
		var b bin.Buffer
		if err := o.o.Encode(&b); err != nil {
			t.Fatal(err)
		}
		v.TL = append(v.TL, tlVector{Name: o.name, Hex: h(b.Buf)})
	}
	var k crypto.Key
	copy(k[:], det(256, "authkey"))
	ak := k.WithID()
	// salt, session_id, msg_id, seq_no, length, body (ping), 12+ bytes of padding: a full plaintext with its padding, so encryption is fixed.
	body := []byte{0xec, 0x77, 0xbe, 0x7a, 1, 2, 3, 4, 5, 6, 7, 8}
	plain := make([]byte, 0, 64)
	plain = append(plain, det(8, "salt")...)
	plain = append(plain, det(8, "session")...)
	plain = append(plain, 0x04, 0, 0, 0, 0x10, 0x20, 0x30, 0x40)
	plain = append(plain, 1, 0, 0, 0, byte(len(body)), 0, 0, 0)
	plain = append(plain, body...)
	plain = append(plain, det(64-len(plain), "padding")...)
	seal := func(side crypto.Side) []byte {
		mk := crypto.MessageKey(ak.Value, plain, side)
		key, iv := crypto.Keys(ak.Value, mk, side)
		blk, _ := aes.NewCipher(key[:])
		out := append(append(append([]byte(nil), ak.ID[:]...), mk[:]...), make([]byte, len(plain))...)
		ige.EncryptBlocks(blk, iv[:], out[24:], plain)
		return out
	}
	v.Crypto = cryptoVector{AuthKey: h(ak.Value[:]), KeyID: h(ak.ID[:]), Plaintext: h(plain), ClientToServer: h(seal(crypto.Client)), ServerToClient: h(seal(crypto.Server))}
	return v
}

func TestVectors(t *testing.T) {
	v := makeVectors(t)
	got, _ := json.MarshalIndent(v, "", "  ")
	got = append(got, '\n')
	if *updateVectors {
		if err := os.WriteFile(vectorPath, got, 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	want, err := os.ReadFile(vectorPath)
	if err != nil {
		t.Fatalf("%v (generate with -update)", err)
	}
	if !bytes.Equal(bytes.ReplaceAll(want, []byte("\r\n"), []byte("\n")), got) {
		t.Fatal("the Go side no longer produces native/testdata/mt_vectors.json: a protocol change (regenerate on purpose with -update and update the Rust client)")
	}
	// The server's own seal of the same plaintext decrypts to it (the server pads randomly, so only the round trip is checked).
	var k crypto.Key
	copy(k[:], det(256, "authkey"))
	s := &Server{}
	key := &authKey{ak: k.WithID()}
	frame, err := s.seal(key, 1, 2, 4, 1, []byte{0xec, 0x77, 0xbe, 0x7a, 9, 9, 9, 9, 9, 9, 9, 9})
	if err != nil {
		t.Fatal(err)
	}
	var mk bin.Int128
	copy(mk[:], frame[8:24])
	aesKey, iv := crypto.Keys(k, mk, crypto.Server)
	blk, _ := aes.NewCipher(aesKey[:])
	plain := make([]byte, len(frame)-24)
	ige.DecryptBlocks(blk, iv[:], plain, frame[24:])
	if want := crypto.MessageKey(k, plain, crypto.Server); want != mk || plain[32] != 0xec {
		t.Fatal("seal does not round-trip")
	}
}
