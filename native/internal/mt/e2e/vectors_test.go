package e2e

// The cross-language vectors of end-to-end direct messages: native/testdata/e2e_vectors.json, made here from fixed seeds and randoms and
// read by the launcher's tests (copied as testdata/e2e_vectors.json). Regenerate with `go test ./internal/mt/e2e -run TestE2EVectors
// -update` only when the protocol changes on purpose; both sides must then agree on the new bytes. Every value is hex unless named
// otherwise; numbers are JSON integers (int64 / int32 as their TL type).
//
//	note                    how the file is made
//	realm                   the realm in the device certificates ("test")
//	p, g                    the pinned group (contract 2.2): p as 256 bytes big-endian, g = 2
//	range[]                 {value (256 bytes), ok}: the safety range check on boundary values (2^1984 - 1, 2^1984, p - 2^1984, p - 2^1984 + 1)
//	admin, participant      one side each:
//	  user_id, device_id      the account id and the chat device id (auth.signIn's device_id)
//	  account_seed            Ed25519 seed (32 bytes) of the account key; account_pub its public key
//	  device_seed             Ed25519 seed of the chat device key; device_pub its public key
//	  device_msg, device_sig  contract 2.1: the certificate's signed bytes and the account key's signature (account.registerDevice)
//	  fresh                   the 256 fresh random bytes of the secret
//	  server_random           messages.getDhConfig's random (the admin's is 32 bytes, right-aligned; the participant's 256)
//	  secret                  a (admin) / b (participant): fresh XOR server_random, mod p, 256 bytes big-endian
//	  public                  g_a / g_b = 2^secret mod p
//	key                     the chat key g_b^a = g_a^b mod p (256 bytes)
//	key_fingerprint         i64 from the last 8 bytes of SHA1(key), little-endian; key_fingerprint_le those 8 bytes
//	request                 contract 2.3: random_id (int32), req_msg, g_a_sig (the admin's device key over req_msg)
//	accept                  contract 2.3: chat_id (int32), acc_msg, g_b_sig (the participant's device key over acc_msg)
//	messages[]              contract 2.4 and 2.7, one sealed by each side:
//	  sender, x               admin (x = 0) or participant (x = 8)
//	  random_bytes, in_seq_no, out_seq_no, random_id, date, text, franking_key   the decryptedMessageLayer and its decryptedMessage
//	  inner                   the TL encoding of that decryptedMessageLayer
//	  padding                 the fixed padding; plaintext = u32LE(len(inner)) || inner || padding
//	  msg_key, aes_key, aes_iv   the derived values
//	  data                    u64LE(key_fingerprint) || msg_key || AES-256-IGE(plaintext): messages.sendEncrypted.data
//	  commitment              HMAC-SHA256(franking_key, "FH2MP-CHAT-FRANK-v1\0" || sender || recipient || random_id || text)
//	tl[]                    {name, hex}: the TL encoding of each new P3 object as built here (constructors of proto/fh2mp.tl)

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"flag"
	"math/big"
	"os"
	"testing"

	"github.com/gotd/td/bin"

	"fh2mp/internal/mt/tl"
)

var updateVectors = flag.Bool("update", false, "rewrite native/testdata/e2e_vectors.json")

const vectorPath = "../../../testdata/e2e_vectors.json"

type rangeVec struct {
	Value string `json:"value"`
	OK    bool   `json:"ok"`
}

type sideVec struct {
	UserID       int64  `json:"user_id"`
	DeviceID     int64  `json:"device_id"`
	AccountSeed  string `json:"account_seed"`
	AccountPub   string `json:"account_pub"`
	DeviceSeed   string `json:"device_seed"`
	DevicePub    string `json:"device_pub"`
	DeviceMsg    string `json:"device_msg"`
	DeviceSig    string `json:"device_sig"`
	Fresh        string `json:"fresh"`
	ServerRandom string `json:"server_random"`
	Secret       string `json:"secret"`
	Public       string `json:"public"`
}

type requestVec struct {
	RandomID int32  `json:"random_id"`
	ReqMsg   string `json:"req_msg"`
	GASig    string `json:"g_a_sig"`
}

type acceptVec struct {
	ChatID int32  `json:"chat_id"`
	AccMsg string `json:"acc_msg"`
	GBSig  string `json:"g_b_sig"`
}

type sealVec struct {
	Sender      string `json:"sender"`
	X           int    `json:"x"`
	RandomBytes string `json:"random_bytes"`
	InSeqNo     int    `json:"in_seq_no"`
	OutSeqNo    int    `json:"out_seq_no"`
	RandomID    int64  `json:"random_id"`
	Date        int    `json:"date"`
	Text        string `json:"text"`
	FrankingKey string `json:"franking_key"`
	Inner       string `json:"inner"`
	Padding     string `json:"padding"`
	Plaintext   string `json:"plaintext"`
	MsgKey      string `json:"msg_key"`
	AESKey      string `json:"aes_key"`
	AESIV       string `json:"aes_iv"`
	Data        string `json:"data"`
	Commitment  string `json:"commitment"`
}

type tlVec struct {
	Name string `json:"name"`
	Hex  string `json:"hex"`
}

type vectorFile struct {
	Note             string     `json:"note"`
	Realm            string     `json:"realm"`
	P                string     `json:"p"`
	G                int        `json:"g"`
	Range            []rangeVec `json:"range"`
	Admin            sideVec    `json:"admin"`
	Participant      sideVec    `json:"participant"`
	Key              string     `json:"key"`
	KeyFingerprint   int64      `json:"key_fingerprint"`
	KeyFingerprintLE string     `json:"key_fingerprint_le"`
	Request          requestVec `json:"request"`
	Accept           acceptVec  `json:"accept"`
	Messages         []sealVec  `json:"messages"`
	TL               []tlVec    `json:"tl"`
}

// det is n bytes expanded from a seed with SHA-256 (the same rule as the chat vectors in internal/mt).
func det(n int, seed string) []byte {
	var out []byte
	for i := 0; len(out) < n; i++ {
		h := sha256.Sum256([]byte(seed + string(rune('a'+i%26)) + string(rune(i))))
		out = append(out, h[:]...)
	}
	return out[:n]
}

var h = hex.EncodeToString

func side(t *testing.T, name string, user, device int64, serverRandom []byte) (sideVec, ed25519.PrivateKey, *big.Int) {
	acct := ed25519.NewKeyFromSeed(det(32, name+"-account"))
	dev := ed25519.NewKeyFromSeed(det(32, name+"-device"))
	devPub := dev.Public().(ed25519.PublicKey)
	fresh := det(256, name+"-fresh")
	secret, err := Secret(fresh, serverRandom)
	if err != nil {
		t.Fatal(err)
	}
	return sideVec{UserID: user, DeviceID: device, AccountSeed: h(acct.Seed()), AccountPub: h(acct.Public().(ed25519.PublicKey)),
		DeviceSeed: h(dev.Seed()), DevicePub: h(devPub), DeviceMsg: h(DeviceMessage("test", device, devPub)),
		DeviceSig: h(SignDevice(acct, "test", device, devPub)), Fresh: h(fresh), ServerRandom: h(serverRandom), Secret: h(Num(secret)),
		Public: h(Public(secret))}, dev, secret
}

func sealed(t *testing.T, key []byte, fp int64, sender Role, from, to int64, name string, in, out int, text string) sealVec {
	rb := det(16, name+"-random-bytes")
	rid := int64(binary.LittleEndian.Uint64(det(8, name+"-random-id")))
	fk := det(32, name+"-franking-key")
	layer := &tl.DecryptedMessageLayer{RandomBytes: rb, Layer: 1, InSeqNo: in, OutSeqNo: out,
		Message: &tl.DecryptedMessage{RandomID: rid, Date: 1_800_000_100, Text: text, FrankingKey: fk}}
	var b bin.Buffer
	if err := layer.Encode(&b); err != nil {
		t.Fatal(err)
	}
	pad := det(PaddingLen(len(b.Buf), 1), name+"-padding")
	plain, err := Plaintext(b.Buf, pad)
	if err != nil {
		t.Fatal(err)
	}
	data, err := SealPlaintext(key, fp, sender, plain)
	if err != nil {
		t.Fatal(err)
	}
	mk := MsgKey(key, sender, plain)
	ak, iv := AESKeys(key, sender, mk)
	return sealVec{Sender: sender.String(), X: sender.x(), RandomBytes: h(rb), InSeqNo: in, OutSeqNo: out, RandomID: rid, Date: 1_800_000_100,
		Text: text, FrankingKey: h(fk), Inner: h(b.Buf), Padding: h(pad), Plaintext: h(plain), MsgKey: h(mk[:]), AESKey: h(ak[:]), AESIV: h(iv[:]),
		Data: h(data), Commitment: h(Commitment(fk, from, to, rid, text))}
}

func makeVectors(t *testing.T) vectorFile {
	const adminID, participantID = 0x9000000000d10, 0x9000000000d03
	const adminDevice, participantDevice = 0x1122334455667788, -0x0123456789abcdef
	v := vectorFile{Note: "frozen; regenerate with go test ./internal/mt/e2e -run TestE2EVectors -update (protocol change only)", Realm: "test",
		P: h(PBytes()), G: G}
	low := new(big.Int).Lsh(big.NewInt(1), 2048-64)
	high := new(big.Int).Sub(P, low)
	for _, r := range []struct {
		v  *big.Int
		ok bool
	}{{new(big.Int).Sub(low, big.NewInt(1)), false}, {low, true}, {high, true}, {new(big.Int).Add(high, big.NewInt(1)), false}} {
		v.Range = append(v.Range, rangeVec{Value: h(Num(r.v)), OK: r.ok})
	}
	var adminDev, partDev ed25519.PrivateKey
	var a, b *big.Int
	v.Admin, adminDev, a = side(t, "admin", adminID, adminDevice, det(32, "server-random-a"))
	v.Participant, partDev, b = side(t, "participant", participantID, participantDevice, det(256, "server-random-b"))
	gA, gB := Public(a), Public(b)
	if CheckPublic(gA) != nil || CheckPublic(gB) != nil {
		t.Fatal("a fixed public value is out of range: pick other seeds")
	}
	key, err := SharedKey(gB, a)
	if err != nil {
		t.Fatal(err)
	}
	if k2, _ := SharedKey(gA, b); !bytes.Equal(key, k2) {
		t.Fatal("g_b^a != g_a^b")
	}
	fp := Fingerprint(key)
	v.Key, v.KeyFingerprint = h(key), fp
	v.KeyFingerprintLE = h(binary.LittleEndian.AppendUint64(nil, uint64(fp)))
	e := Ends{Admin: adminID, AdminDevice: adminDevice, Participant: participantID, ParticipantDevice: participantDevice}
	const randomID, chatID = int32(-559038737), int32(4242)
	req := RequestMessage(e, randomID, gA)
	v.Request = requestVec{RandomID: randomID, ReqMsg: h(req), GASig: h(ed25519.Sign(adminDev, req))}
	acc := AcceptMessage(chatID, e, gA, gB, fp)
	v.Accept = acceptVec{ChatID: chatID, AccMsg: h(acc), GBSig: h(ed25519.Sign(partDev, acc))}
	v.Messages = append(v.Messages,
		sealed(t, key, fp, Admin, adminID, participantID, "msg-admin", 1, 0, "hello over the secret chat ✓"),
		sealed(t, key, fp, Participant, participantID, adminID, "msg-participant", 2, 1, "echo: hello over the secret chat ✓"))

	devPub, _ := hex.DecodeString(v.Admin.DevicePub)
	acctPub, _ := hex.DecodeString(v.Admin.AccountPub)
	devSig, _ := hex.DecodeString(v.Admin.DeviceSig)
	gASig, _ := hex.DecodeString(v.Request.GASig)
	gBSig, _ := hex.DecodeString(v.Accept.GBSig)
	data, _ := hex.DecodeString(v.Messages[0].Data)
	commit, _ := hex.DecodeString(v.Messages[0].Commitment)
	objs := []struct {
		name string
		o    bin.Encoder
	}{
		{"chatDevice", &tl.ChatDevice{UserID: adminID, DeviceID: adminDevice, Pubkey: devPub, AccountKey: acctPub, Signature: devSig, Date: 1_800_000_000}},
		{"users.devices", &tl.UsersDevices{Devices: []tl.ChatDevice{{UserID: adminID, DeviceID: adminDevice, Pubkey: devPub, AccountKey: acctPub,
			Signature: devSig, Date: 1_800_000_000}}}},
		{"messages.dhConfig", &tl.MessagesDhConfig{G: G, P: PBytes(), Version: GroupVersion, Random: det(32, "server-random-a")}},
		{"messages.dhConfigNotModified", &tl.MessagesDhConfigNotModified{Random: det(32, "server-random-a")}},
		{"encryptedChatWaiting", &tl.EncryptedChatWaiting{ID: int(chatID), Date: 1_800_000_001, AdminID: adminID, AdminDevice: adminDevice,
			ParticipantID: participantID, ParticipantDevice: participantDevice}},
		{"encryptedChatRequested", &tl.EncryptedChatRequested{ID: int(chatID), Date: 1_800_000_001, AdminID: adminID, AdminDevice: adminDevice,
			ParticipantID: participantID, ParticipantDevice: participantDevice, GA: gA, GASig: gASig}},
		{"encryptedChat", &tl.EncryptedChat{ID: int(chatID), Date: 1_800_000_001, AdminID: adminID, AdminDevice: adminDevice,
			ParticipantID: participantID, ParticipantDevice: participantDevice, GAOrB: gB, Sig: gBSig, KeyFingerprint: fp}},
		{"encryptedChatDiscarded", &tl.EncryptedChatDiscarded{ID: int(chatID)}},
		{"encryptedMessage", &tl.EncryptedMessage{RandomID: v.Messages[0].RandomID, ChatID: int(chatID), Date: 1_800_000_002, Bytes: data, Commitment: commit}},
		{"messages.sentEncryptedMessage", &tl.MessagesSentEncryptedMessage{Date: 1_800_000_002}},
		{"updateEncryption", &tl.UpdateEncryption{Chat: &tl.EncryptedChatDiscarded{ID: int(chatID)}, Date: 1_800_000_003}},
		{"updateNewEncryptedMessage", &tl.UpdateNewEncryptedMessage{Message: tl.EncryptedMessage{RandomID: v.Messages[0].RandomID, ChatID: int(chatID),
			Date: 1_800_000_002, Bytes: data, Commitment: commit}}},
		{"updateChatDevice", &tl.UpdateChatDevice{UserID: adminID, DeviceID: adminDevice}},
		{"account.registerDevice", &tl.AccountRegisterDeviceRequest{Pubkey: devPub, Signature: devSig}},
		{"users.getDevices", &tl.UsersGetDevicesRequest{UserID: participantID}},
		{"messages.getDhConfig", &tl.MessagesGetDhConfigRequest{Version: 0, RandomLength: 256}},
		{"messages.requestEncryption", &tl.MessagesRequestEncryptionRequest{UserID: participantID, DeviceID: participantDevice, RandomID: int(randomID),
			GA: gA, GASig: gASig}},
		{"messages.acceptEncryption", &tl.MessagesAcceptEncryptionRequest{ChatID: int(chatID), GB: gB, KeyFingerprint: fp, GBSig: gBSig}},
		{"messages.discardEncryption", &tl.MessagesDiscardEncryptionRequest{ChatID: int(chatID)}},
		{"messages.sendEncrypted", &tl.MessagesSendEncryptedRequest{ChatID: int(chatID), RandomID: v.Messages[0].RandomID, Data: data, Commitment: commit}},
		{"messages.ackEncrypted", &tl.MessagesAckEncryptedRequest{MaxPts: 17}},
		{"messages.reportEncrypted", &tl.MessagesReportEncryptedRequest{ChatID: int(chatID), RandomID: v.Messages[0].RandomID,
			Text: v.Messages[0].Text, FrankingKey: det(32, "msg-admin-franking-key"), Reason: "spam"}},
	}
	actions := []struct {
		name string
		a    tl.DecryptedMessageActionClass
	}{
		{"readMessages", &tl.DecryptedMessageActionReadMessages{RandomIDs: []int64{1, -2}}},
		{"typing", &tl.DecryptedMessageActionTyping{}},
		{"resend", &tl.DecryptedMessageActionResend{StartSeqNo: 4, EndSeqNo: 8}},
		{"requestKey", &tl.DecryptedMessageActionRequestKey{ExchangeID: -77, GA: gA}},
		{"acceptKey", &tl.DecryptedMessageActionAcceptKey{ExchangeID: -77, GB: gB, KeyFingerprint: fp}},
		{"commitKey", &tl.DecryptedMessageActionCommitKey{ExchangeID: -77, KeyFingerprint: fp}},
		{"abortKey", &tl.DecryptedMessageActionAbortKey{ExchangeID: -77}},
		{"noop", &tl.DecryptedMessageActionNoop{}},
	}
	for _, a := range actions {
		objs = append(objs, struct {
			name string
			o    bin.Encoder
		}{"decryptedMessageLayer/service/" + a.name, &tl.DecryptedMessageLayer{RandomBytes: det(15, "service-random-bytes"), Layer: 1, InSeqNo: 3,
			OutSeqNo: 2, Message: &tl.DecryptedMessageService{RandomID: 99, Action: a.a}}})
	}
	for _, o := range objs {
		var b bin.Buffer
		if err := o.o.Encode(&b); err != nil {
			t.Fatal(err)
		}
		v.TL = append(v.TL, tlVec{Name: o.name, Hex: h(b.Buf)})
	}
	return v
}

func TestE2EVectors(t *testing.T) {
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
		t.Fatal("the Go side no longer produces native/testdata/e2e_vectors.json: a protocol change (regenerate on purpose with -update and give the launcher the new file)")
	}
	// The file also checks itself: each sealed message opens with the key as its sender and its commitment matches.
	key, _ := hex.DecodeString(v.Key)
	for _, m := range v.Messages {
		data, _ := hex.DecodeString(m.Data)
		inner, _ := hex.DecodeString(m.Inner)
		role := Admin
		if m.Sender == "participant" {
			role = Participant
		}
		got, err := Open(key, v.KeyFingerprint, role, data)
		if err != nil || !bytes.Equal(got, inner) {
			t.Fatalf("%s's vector does not open: %v", m.Sender, err)
		}
	}
	devPub, _ := hex.DecodeString(v.Participant.DevicePub)
	acctPub, _ := hex.DecodeString(v.Participant.AccountPub)
	sig, _ := hex.DecodeString(v.Participant.DeviceSig)
	if !VerifyDevice(acctPub, v.Realm, v.Participant.DeviceID, devPub, sig) {
		t.Fatal("the participant's device certificate does not verify")
	}
}
