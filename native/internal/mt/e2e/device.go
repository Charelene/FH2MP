package e2e

import (
	"crypto/ed25519"
	"encoding/binary"
)

// DeviceTag starts every chat device certificate.
const DeviceTag = "FH2MP-CHAT-DEVICE-v1\x00"

// DeviceMessage is what the account key signs to vouch for a chat device (contract 2.1):
// "FH2MP-CHAT-DEVICE-v1\0" || realm || "\0" || u64LE(device_id) || device_pub(32).
func DeviceMessage(realm string, deviceID int64, devicePub []byte) []byte {
	m := make([]byte, 0, len(DeviceTag)+len(realm)+1+8+len(devicePub))
	m = append(m, DeviceTag...)
	m = append(m, realm...)
	m = append(m, 0)
	m = binary.LittleEndian.AppendUint64(m, uint64(deviceID))
	return append(m, devicePub...)
}

// SignDevice is the account's signature over DeviceMessage.
func SignDevice(account ed25519.PrivateKey, realm string, deviceID int64, devicePub []byte) []byte {
	return ed25519.Sign(account, DeviceMessage(realm, deviceID, devicePub))
}

// VerifyDevice checks a chat device certificate against the account's public key.
func VerifyDevice(accountPub []byte, realm string, deviceID int64, devicePub, sig []byte) bool {
	if len(accountPub) != ed25519.PublicKeySize || len(devicePub) != ed25519.PublicKeySize || len(sig) != ed25519.SignatureSize {
		return false
	}
	return ed25519.Verify(accountPub, DeviceMessage(realm, deviceID, devicePub), sig)
}
