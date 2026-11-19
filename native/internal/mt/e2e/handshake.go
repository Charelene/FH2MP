package e2e

import (
	"crypto/ed25519"
	"math/big"
)

// Request is the admin's half of a new secret chat: the secret a (kept), g_a and its signature (sent with messages.requestEncryption).
type Request struct {
	A   *big.Int
	GA  []byte
	Sig []byte
}

// MakeRequest makes a from 256 fresh random bytes and the server's random (messages.getDhConfig), and signs g_a with the admin's device key.
func MakeRequest(adminDevice ed25519.PrivateKey, e Ends, randomID int32, fresh, serverRandom []byte) (*Request, error) {
	a, err := Secret(fresh, serverRandom)
	if err != nil {
		return nil, err
	}
	gA := Public(a)
	if err := CheckPublic(gA); err != nil {
		return nil, err // a value this unlikely is made again by the caller with new random bytes
	}
	return &Request{A: a, GA: gA, Sig: ed25519.Sign(adminDevice, RequestMessage(e, randomID, gA))}, nil
}

// Accept is the participant's half: b's public value, the chat key, its fingerprint and the signature (messages.acceptEncryption).
type Accept struct {
	GB          []byte
	Key         []byte
	Fingerprint int64
	Sig         []byte
}

// AcceptRequest checks the admin's signature on g_a against the admin's pinned device key, makes b and the key, and signs the accept.
func AcceptRequest(participantDevice ed25519.PrivateKey, adminDevicePub []byte, chatID int32, e Ends, randomID int32, gA, gASig, fresh,
	serverRandom []byte) (*Accept, error) {
	if !VerifyRequest(adminDevicePub, e, randomID, gA, gASig) {
		return nil, refuse("g_a signature does not verify against the admin's device key")
	}
	b, err := Secret(fresh, serverRandom)
	if err != nil {
		return nil, err
	}
	key, err := SharedKey(gA, b)
	if err != nil {
		return nil, err
	}
	gB := Public(b)
	if err := CheckPublic(gB); err != nil {
		return nil, err
	}
	fp := Fingerprint(key)
	return &Accept{GB: gB, Key: key, Fingerprint: fp, Sig: ed25519.Sign(participantDevice, AcceptMessage(chatID, e, gA, gB, fp))}, nil
}

// FinishRequest is the admin's end once the chat was accepted: the participant's signature checked against its pinned device key, the key
// made from g_b, and the fingerprint compared with the one the participant announced.
func FinishRequest(r *Request, participantDevicePub []byte, chatID int32, e Ends, gB []byte, fingerprint int64, gBSig []byte) ([]byte, error) {
	if !VerifyAccept(participantDevicePub, chatID, e, r.GA, gB, fingerprint, gBSig) {
		return nil, refuse("g_b signature does not verify against the participant's device key")
	}
	key, err := SharedKey(gB, r.A)
	if err != nil {
		return nil, err
	}
	if Fingerprint(key) != fingerprint {
		return nil, refuse("key fingerprint does not match the participant's")
	}
	return key, nil
}
