package e2e

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"math/big"
	"testing"

	"github.com/gotd/td/bin"
	"github.com/gotd/td/crypto"

	"fh2mp/internal/mt/tl"
)

func seeded(seed string) ed25519.PrivateKey {
	h := sha256.Sum256([]byte(seed))
	return ed25519.NewKeyFromSeed(h[:])
}

func randBytes(n int) []byte {
	b := make([]byte, n)
	rand.Read(b)
	return b
}

// The pinned group: p is the RFC 3526 group 14 prime (2048 bits, top and bottom 64 bits all ones), a safe prime, and g = 2 generates the
// subgroup of prime order (p-1)/2 (gotd's crypto.CheckDH: p mod 8 = 7 for g = 2, p and (p-1)/2 prime).
func TestGroup(t *testing.T) {
	if P.BitLen() != 2048 {
		t.Fatalf("p has %d bits", P.BitLen())
	}
	ones := new(big.Int).SetUint64(^uint64(0))
	if low := new(big.Int).And(P, ones); low.Cmp(ones) != 0 {
		t.Fatalf("bottom 64 bits of p: %x", low)
	}
	if high := new(big.Int).Rsh(P, 2048-64); high.Cmp(ones) != 0 {
		t.Fatalf("top 64 bits of p: %x", high)
	}
	if err := crypto.CheckDH(G, P); err != nil {
		t.Fatalf("CheckDH(2, p): %v", err)
	}
	if b := PBytes(); len(b) != 256 || b[0] != 0xff || b[255] != 0xff {
		t.Fatalf("PBytes: %d bytes", len(b))
	}
}

func TestRange(t *testing.T) {
	low := new(big.Int).Lsh(big.NewInt(1), 2048-64)
	high := new(big.Int).Sub(P, low)
	for _, c := range []struct {
		v    *big.Int
		want bool
	}{
		{big.NewInt(0), false}, {big.NewInt(1), false}, {big.NewInt(2), false},
		{new(big.Int).Sub(low, big.NewInt(1)), false}, {low, true}, {high, true}, {new(big.Int).Add(high, big.NewInt(1)), false},
		{new(big.Int).Sub(P, big.NewInt(1)), false}, {P, false},
	} {
		if got := InRange(c.v); got != c.want {
			t.Errorf("InRange(%x...) = %v, want %v", Num(c.v)[:4], got, c.want)
		}
	}
	if CheckPublic(Num(low)[1:]) == nil {
		t.Error("a 255-byte value passed")
	}
	if CheckPublic(Num(low)) != nil {
		t.Error("2^1984 refused")
	}
}

// key_fingerprint is the auth_key_id rule of MTProto (the low 64 bits of SHA1(key), little-endian), as gotd computes it.
func TestFingerprintMatchesGotd(t *testing.T) {
	key := randBytes(256)
	var k crypto.Key
	copy(k[:], key)
	id := k.WithID().ID
	if got, want := Fingerprint(key), int64(binary.LittleEndian.Uint64(id[:])); got != want {
		t.Fatalf("fingerprint %x, gotd's key id %x", got, want)
	}
}

func TestSecretRightAligned(t *testing.T) {
	fresh := make([]byte, 256)
	fresh[0] = 0x01
	a, err := Secret(fresh, []byte{0xaa, 0xbb})
	if err != nil {
		t.Fatal(err)
	}
	want := make([]byte, 256)
	want[0], want[254], want[255] = 0x01, 0xaa, 0xbb
	if !bytes.Equal(Num(a), want) {
		t.Fatalf("server random not XORed into the last bytes: %x", Num(a)[250:])
	}
	if _, err := Secret(fresh[:255], nil); err == nil {
		t.Fatal("255 fresh bytes taken")
	}
	if _, err := Secret(fresh, make([]byte, 257)); err == nil {
		t.Fatal("257 bytes of server random taken")
	}
	// A value at or above p is taken mod p.
	allOnes := bytes.Repeat([]byte{0xff}, 256)
	a, _ = Secret(allOnes, nil)
	if a.Cmp(P) >= 0 {
		t.Fatal("a not reduced mod p")
	}
}

func TestDeviceCertificate(t *testing.T) {
	acct := seeded("account")
	dev := seeded("device").Public().(ed25519.PublicKey)
	msg := DeviceMessage("test", 0x0102030405060708, dev)
	want := append(append([]byte("FH2MP-CHAT-DEVICE-v1\x00test\x00"), 8, 7, 6, 5, 4, 3, 2, 1), dev...)
	if !bytes.Equal(msg, want) {
		t.Fatalf("device_msg %x", msg)
	}
	sig := SignDevice(acct, "test", 0x0102030405060708, dev)
	pub := acct.Public().(ed25519.PublicKey)
	if !VerifyDevice(pub, "test", 0x0102030405060708, dev, sig) {
		t.Fatal("own certificate refused")
	}
	bad := append([]byte(nil), sig...)
	bad[10] ^= 1
	for name, ok := range map[string]bool{
		"other realm":     VerifyDevice(pub, "prod", 0x0102030405060708, dev, sig),
		"other device id": VerifyDevice(pub, "test", 0x0102030405060709, dev, sig),
		"other key":       VerifyDevice(pub, "test", 0x0102030405060708, seeded("x").Public().(ed25519.PublicKey), sig),
		"flipped bit":     VerifyDevice(pub, "test", 0x0102030405060708, dev, bad),
		"other account":   VerifyDevice(seeded("y").Public().(ed25519.PublicKey), "test", 0x0102030405060708, dev, sig),
		"short key":       VerifyDevice(pub, "test", 0x0102030405060708, dev[:31], sig),
	} {
		if ok {
			t.Errorf("%s: certificate taken", name)
		}
	}
}

func TestHandshake(t *testing.T) {
	adminDev, partDev := seeded("admin-dev"), seeded("part-dev")
	adminPub, partPub := adminDev.Public().(ed25519.PublicKey), partDev.Public().(ed25519.PublicKey)
	e := Ends{Admin: 10, AdminDevice: 11, Participant: 20, ParticipantDevice: 21}
	req, err := MakeRequest(adminDev, e, randBytes(256), randBytes(64))
	if err != nil {
		t.Fatal(err)
	}
	if len(req.GA) != 256 || CheckPublic(req.GA) != nil {
		t.Fatal("g_a out of range")
	}
	acc, err := AcceptRequest(partDev, adminPub, 5, e, req.GA, req.Sig, randBytes(256), nil)
	if err != nil {
		t.Fatal(err)
	}
	key, err := FinishRequest(req, partPub, 5, e, acc.GB, acc.Fingerprint, acc.Sig)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(key, acc.Key) || len(key) != 256 || Fingerprint(key) != acc.Fingerprint {
		t.Fatal("the two sides made different keys")
	}
	// Tampering: another admin device, a flipped g_a, the wrong device key, another participant device, another chat id, a wrong fingerprint.
	e1 := e
	e1.AdminDevice = 12
	if _, err := AcceptRequest(partDev, adminPub, 5, e1, req.GA, req.Sig, randBytes(256), nil); err == nil {
		t.Error("g_a_sig taken for another admin device")
	}
	ga := append([]byte(nil), req.GA...)
	ga[100] ^= 1
	if _, err := AcceptRequest(partDev, adminPub, 5, e, ga, req.Sig, randBytes(256), nil); err == nil {
		t.Error("tampered g_a taken")
	}
	if _, err := AcceptRequest(partDev, partPub, 5, e, req.GA, req.Sig, randBytes(256), nil); err == nil {
		t.Error("g_a_sig taken against the wrong device key")
	}
	e2 := e
	e2.ParticipantDevice = 22
	if _, err := AcceptRequest(partDev, adminPub, 5, e2, req.GA, req.Sig, randBytes(256), nil); err == nil {
		t.Error("g_a_sig taken for another participant device")
	}
	if _, err := FinishRequest(req, partPub, 6, e, acc.GB, acc.Fingerprint, acc.Sig); err == nil {
		t.Error("g_b_sig taken for another chat id")
	}
	if _, err := FinishRequest(req, adminPub, 5, e, acc.GB, acc.Fingerprint, acc.Sig); err == nil {
		t.Error("g_b_sig taken against the wrong device key")
	}
	wrongFP := acc.Fingerprint + 1
	sig := ed25519.Sign(partDev, AcceptMessage(5, e, req.GA, acc.GB, wrongFP))
	if _, err := FinishRequest(req, partPub, 5, e, acc.GB, wrongFP, sig); err == nil {
		t.Error("a signed but wrong fingerprint taken")
	}
	// Out-of-range public values are refused before any use.
	if _, err := SharedKey(Num(big.NewInt(1)), req.A); err == nil {
		t.Error("g_b = 1 taken")
	}
	if _, err := SharedKey(PBytes(), req.A); err == nil {
		t.Error("g_b = p taken")
	}
}

func TestRequestAcceptLayout(t *testing.T) {
	e := Ends{Admin: 1, AdminDevice: 2, Participant: 3, ParticipantDevice: 4}
	ga, gb := bytes.Repeat([]byte{0xa1}, 256), bytes.Repeat([]byte{0xb2}, 256)
	req := RequestMessage(e, ga)
	if string(req[:25]) != "FH2MP-CHAT-DH-v1\x00request\x00" || len(req) != 25+32+256 || req[25] != 1 || req[33] != 2 || req[41] != 3 || req[49] != 4 ||
		req[56] != 0 || req[57] != 0xa1 || req[len(req)-1] != 0xa1 {
		t.Fatalf("req_msg %x", req[:64])
	}
	acc := AcceptMessage(9, e, ga, gb, 0x0807060504030201)
	if string(acc[:24]) != "FH2MP-CHAT-DH-v1\x00accept\x00" || len(acc) != 24+4+32+512+8 || acc[24] != 9 || acc[28] != 1 || acc[60] != 0xa1 ||
		acc[316] != 0xb2 || !bytes.Equal(acc[572:], []byte{1, 2, 3, 4, 5, 6, 7, 8}) {
		t.Fatalf("acc_msg layout")
	}
}

func layerBytes(t *testing.T, in, out int, text string) []byte {
	var b bin.Buffer
	l := &tl.DecryptedMessageLayer{RandomBytes: randBytes(15), Layer: 1, InSeqNo: in, OutSeqNo: out,
		Message: &tl.DecryptedMessage{RandomID: 1, Date: 2, Text: text, FrankingKey: randBytes(32)}}
	if err := l.Encode(&b); err != nil {
		t.Fatal(err)
	}
	return b.Buf
}

func TestSealOpen(t *testing.T) {
	key := randBytes(256)
	fp := Fingerprint(key)
	inner := layerBytes(t, 1, 0, "hello")
	for _, role := range []Role{Admin, Participant} {
		data, err := SealRandom(key, fp, role, inner)
		if err != nil {
			t.Fatal(err)
		}
		got, err := Open(key, fp, role, data)
		if err != nil || !bytes.Equal(got, inner) {
			t.Fatalf("%s: round trip %v", role, err)
		}
		// The message key and the AES key and iv are MTProto 2.0's: the admin's x = 0 is gotd's client side, the participant's x = 8 its server side.
		var k crypto.Key
		copy(k[:], key)
		side := crypto.Client
		if role == Participant {
			side = crypto.Server
		}
		plain, _ := Plaintext(inner, make([]byte, PaddingLen(len(inner), 0)))
		mk := MsgKey(key, role, plain)
		if gmk := crypto.MessageKey(k, plain, side); !bytes.Equal(gmk[:], mk[:]) {
			t.Fatalf("%s: msg_key differs from gotd's MTProto 2.0", role)
		}
		ak, iv := AESKeys(key, role, mk)
		gk, giv := crypto.Keys(k, mk, side)
		if ak != gk || iv != giv {
			t.Fatalf("%s: aes key/iv differ from gotd's", role)
		}
		// Opened as the other role, with another key, or after any bit flip: refused.
		if _, err := Open(key, fp, role.Other(), data); err == nil {
			t.Errorf("%s: opened with the other side's x", role)
		}
		other := randBytes(256)
		if _, err := Open(other, fp, role, data); err == nil {
			t.Errorf("%s: opened with another key", role)
		}
		for _, i := range []int{0, 8, 23, 24, len(data) - 1} {
			bad := append([]byte(nil), data...)
			bad[i] ^= 0x40
			if _, err := Open(key, fp, role, bad); err == nil {
				t.Errorf("%s: byte %d flipped and still opened", role, i)
			}
		}
		if _, err := Open(key, fp, role, data[:len(data)-16]); err == nil {
			t.Errorf("%s: truncated message opened", role)
		}
	}
	// Plaintext rules: padding 12..1024, whole blocks.
	if _, err := Plaintext(inner, make([]byte, 11)); err == nil {
		t.Error("11 bytes of padding taken")
	}
	if _, err := Plaintext(make([]byte, 4), make([]byte, 1032)); err == nil {
		t.Error("1032 bytes of padding taken")
	}
	if _, err := Plaintext(inner, make([]byte, PaddingLen(len(inner), 0)+1)); err == nil {
		t.Error("plaintext that is not whole blocks taken")
	}
	if n := PaddingLen(len(inner), 1000); n > 1024 || (4+len(inner)+n)%16 != 0 {
		t.Errorf("PaddingLen with many extra blocks: %d", n)
	}
	// A plaintext whose length prefix leaves less than 12 bytes of padding is refused even with a correct msg_key.
	p := make([]byte, 32)
	p[0] = 20 // 4 + 20 = 24 > 32 - 12
	data, _ := SealPlaintext(key, fp, Admin, p)
	if _, err := Open(key, fp, Admin, data); err == nil {
		t.Error("length prefix past the padding taken")
	}
	big := make([]byte, 4+16+1040)
	big[0] = 16 // 1040 bytes of padding
	data, _ = SealPlaintext(key, fp, Admin, big)
	if _, err := Open(key, fp, Admin, data); err == nil {
		t.Error("1040 bytes of padding taken")
	}
}

func TestSeq(t *testing.T) {
	a, p := Seq{Role: Admin}, Seq{Role: Participant}
	// The admin sends two, the participant takes them, answers one, the admin takes it.
	in, out := a.Next()
	if in != 1 || out != 0 {
		t.Fatalf("admin first: in %d out %d", in, out)
	}
	if v, _, _ := p.Check(in, out); v != SeqOK {
		t.Fatalf("participant on admin's first: %s", v)
	}
	p.Take()
	in2, out2 := a.Next()
	if in2 != 1 || out2 != 2 {
		t.Fatalf("admin second: in %d out %d", in2, out2)
	}
	if v, _, _ := p.Check(in, out); v != SeqRepeat {
		t.Fatalf("repeat: %s", v)
	}
	p.Take() // the second
	pin, pout := p.Next()
	if pin != 4 || pout != 1 {
		t.Fatalf("participant first: in %d out %d", pin, pout)
	}
	if v, _, _ := a.Check(pin, pout); v != SeqOK {
		t.Fatalf("admin on participant's first: %s", v)
	}
	a.Take()
	// A gap: the participant misses the admin's 3rd and 4th (out 4, 6) and sees the 5th (out 8).
	a.Next()
	a.Next()
	in5, out5 := a.Next()
	v, s, e := p.Check(in5, out5)
	if v != SeqGap || s != 4 || e != 6 {
		t.Fatalf("gap: %s %d..%d", v, s, e)
	}
	// The peer claiming to have received more than was sent, or the wrong parity: broken.
	if v, _, _ := a.Check(2*a.Sent+0+2, 3); v != SeqBroken {
		t.Fatalf("in_seq_no past what was sent: %s", v)
	}
	if v, _, _ := a.Check(1, 2); v != SeqBroken {
		t.Fatalf("participant out_seq_no with the admin's parity: %s", v)
	}
}

func TestCommitment(t *testing.T) {
	fk := randBytes(32)
	c := Commitment(fk, 1, 2, 3, "report me")
	if len(c) != 32 || !CheckCommitment(c, fk, 1, 2, 3, "report me") {
		t.Fatal("own commitment refused")
	}
	for name, ok := range map[string]bool{
		"other text":      CheckCommitment(c, fk, 1, 2, 3, "report me!"),
		"other sender":    CheckCommitment(c, fk, 9, 2, 3, "report me"),
		"swapped ends":    CheckCommitment(c, fk, 2, 1, 3, "report me"),
		"other random_id": CheckCommitment(c, fk, 1, 2, 4, "report me"),
		"other key":       CheckCommitment(c, randBytes(32), 1, 2, 3, "report me"),
		"short key":       CheckCommitment(Commitment(fk[:31], 1, 2, 3, "x"), fk[:31], 1, 2, 3, "x"),
		"short commit":    CheckCommitment(c[:31], fk, 1, 2, 3, "report me"),
	} {
		if ok {
			t.Errorf("%s: commitment matched", name)
		}
	}
}
