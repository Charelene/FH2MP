package e2e

import (
	"crypto/ed25519"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"math/big"
	"strings"
)

// The pinned group of every secret chat (contract 2.2): RFC 3526 group 14, the 2048-bit MODP group, generator 2. The hex of p is copied
// from RFC 3526 section 3 as the contract quotes it; dh_test.go checks that it is a safe 2048-bit prime with g = 2 a generator of the
// prime-order subgroup and that its top and bottom 64 bits are all ones.
const (
	G            = 2
	GroupVersion = 1 // messages.getDhConfig's version of this group
	// NumLen is the length of a DH value on the wire: big-endian, left-padded with zeros.
	NumLen = 256
)

const rfc3526Group14 = `
FFFFFFFF FFFFFFFF C90FDAA2 2168C234 C4C6628B 80DC1CD1 29024E08 8A67CC74 020BBEA6 3B139B22 514A0879 8E3404DD
EF9519B3 CD3A431B 302B0A6D F25F1437 4FE1356D 6D51C245 E485B576 625E7EC6 F44C42E9 A637ED6B 0BFF5CB6 F406B7ED
EE386BFB 5A899FA5 AE9F2411 7C4B1FE6 49286651 ECE45B3D C2007CB8 A163BF05 98DA4836 1C55D39A 69163FA8 FD24CF5F
83655D23 DCA3AD96 1C62F356 208552BB 9ED52907 7096966D 670C354E 4ABC9804 F1746C08 CA18217C 32905E46 2E36CE3B
E39E772C 180E8603 9B2783A2 EC07A28F B5C55DF0 6F4C52C9 DE2BCBF6 95581718 3995497C EA956AE5 15D22618 98FA0510
15728E5A 8AACAA68 FFFFFFFF FFFFFFFF`

// P is the group's prime.
var P = func() *big.Int {
	b, err := hex.DecodeString(strings.Join(strings.Fields(rfc3526Group14), ""))
	if err != nil {
		panic(err)
	}
	return new(big.Int).SetBytes(b)
}()

var (
	bigG = big.NewInt(G)
	// The safety range of a received g_a / g_b (contract 2.2): 1 < v < p-1 and 2^(2048-64) <= v <= p - 2^(2048-64).
	rangeLow  = new(big.Int).Lsh(big.NewInt(1), 2048-64)
	rangeHigh = new(big.Int).Sub(P, rangeLow)
	pMinus1   = new(big.Int).Sub(P, big.NewInt(1))
)

// PBytes is p as messages.dhConfig carries it (256 bytes, big-endian).
func PBytes() []byte { return Num(P) }

// Num writes v as a 256-byte big-endian number.
func Num(v *big.Int) []byte { return v.FillBytes(make([]byte, NumLen)) }

// InRange is the contract's range check of a public value.
func InRange(v *big.Int) bool {
	one := big.NewInt(1)
	return v.Cmp(one) > 0 && v.Cmp(pMinus1) < 0 && v.Cmp(rangeLow) >= 0 && v.Cmp(rangeHigh) <= 0
}

// CheckPublic checks a g_a / g_b as received: 256 bytes and inside the safety range.
func CheckPublic(b []byte) error {
	if len(b) != NumLen {
		return refuse("public value of %d bytes, %d expected", len(b), NumLen)
	}
	if !InRange(new(big.Int).SetBytes(b)) {
		return refuse("public value outside the safety range")
	}
	return nil
}

// Secret makes a = (256 fresh random bytes XOR the server's random, right-aligned when shorter) read big-endian, mod p (contract 2.2).
func Secret(fresh, serverRandom []byte) (*big.Int, error) {
	if len(fresh) != NumLen {
		return nil, refuse("secret from %d fresh bytes, %d expected", len(fresh), NumLen)
	}
	if len(serverRandom) > NumLen {
		return nil, refuse("server random of %d bytes, at most %d", len(serverRandom), NumLen)
	}
	b := append([]byte(nil), fresh...)
	off := NumLen - len(serverRandom)
	for i, r := range serverRandom {
		b[off+i] ^= r
	}
	a := new(big.Int).SetBytes(b)
	return a.Mod(a, P), nil
}

// Public is g^a mod p as 256 bytes.
func Public(a *big.Int) []byte { return Num(new(big.Int).Exp(bigG, a, P)) }

// SharedKey is the chat key peer^a mod p (256 bytes); the peer's value is range-checked first.
func SharedKey(peer []byte, a *big.Int) ([]byte, error) {
	if err := CheckPublic(peer); err != nil {
		return nil, err
	}
	return Num(new(big.Int).Exp(new(big.Int).SetBytes(peer), a, P)), nil
}

// Fingerprint is the key's id: the last 8 bytes of SHA1(key) read as a little-endian i64.
func Fingerprint(key []byte) int64 {
	h := sha1.Sum(key)
	return int64(binary.LittleEndian.Uint64(h[12:20]))
}

const (
	requestTag = "FH2MP-CHAT-DH-v1\x00request\x00"
	acceptTag  = "FH2MP-CHAT-DH-v1\x00accept\x00"
)

// Ends names the two sides of a secret chat: the admin made the request, the participant accepts it.
type Ends struct {
	Admin, AdminDevice, Participant, ParticipantDevice int64
}

func (e Ends) append(m []byte) []byte {
	m = binary.LittleEndian.AppendUint64(m, uint64(e.Admin))
	m = binary.LittleEndian.AppendUint64(m, uint64(e.AdminDevice))
	m = binary.LittleEndian.AppendUint64(m, uint64(e.Participant))
	return binary.LittleEndian.AppendUint64(m, uint64(e.ParticipantDevice))
}

// RequestMessage is what the admin's device key signs for messages.requestEncryption (contract 2.3, as amended):
// tag || u64LE(admin_id) || u64LE(admin_device) || u64LE(participant_id) || u64LE(participant_device) || g_a(256). There is no random_id in
// it: the participant only receives encryptedChatRequested, which does not carry one, and must be able to rebuild the message.
func RequestMessage(e Ends, gA []byte) []byte {
	m := append([]byte(nil), requestTag...)
	m = e.append(m)
	return append(m, gA...)
}

// AcceptMessage is what the participant's device key signs for messages.acceptEncryption (contract 2.3):
// tag || i32LE(chat_id) || the four ids || g_a(256) || g_b(256) || u64LE(key_fingerprint).
func AcceptMessage(chatID int32, e Ends, gA, gB []byte, fingerprint int64) []byte {
	m := append([]byte(nil), acceptTag...)
	m = binary.LittleEndian.AppendUint32(m, uint32(chatID))
	m = e.append(m)
	m = append(m, gA...)
	m = append(m, gB...)
	return binary.LittleEndian.AppendUint64(m, uint64(fingerprint))
}

// VerifyRequest checks g_a_sig against the admin's device key.
func VerifyRequest(adminDevicePub []byte, e Ends, gA, sig []byte) bool {
	return verify(adminDevicePub, RequestMessage(e, gA), sig)
}

// VerifyAccept checks g_b_sig against the participant's device key.
func VerifyAccept(participantDevicePub []byte, chatID int32, e Ends, gA, gB []byte, fingerprint int64, sig []byte) bool {
	return verify(participantDevicePub, AcceptMessage(chatID, e, gA, gB, fingerprint), sig)
}

func verify(pub, msg, sig []byte) bool {
	return len(pub) == ed25519.PublicKeySize && len(sig) == ed25519.SignatureSize && ed25519.Verify(pub, msg, sig)
}
