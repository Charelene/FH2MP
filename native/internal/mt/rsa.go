package mt

import (
	"crypto/aes"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"fmt"
	"math/big"
	"os"

	"github.com/gotd/ige"
	"github.com/gotd/td/crypto"
)

// ServerKey is the server's RSA key of the auth-key exchange. Clients pin its fingerprint (the low 64 bits of SHA1 over the TL-serialized n, e).
type ServerKey struct {
	priv *rsa.PrivateKey
	fp   int64
}

// LoadServerKey reads a PKCS#1 or PKCS#8 PEM private key (FH2MP_MT_RSA_KEY); 2048 bits, as RSA_PAD needs 256-byte blocks.
func LoadServerKey(path string) (*ServerKey, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return parseServerKey(raw)
}

// ParseServerKey is LoadServerKey on PEM bytes.
func ParseServerKey(raw []byte) (*ServerKey, error) { return parseServerKey(raw) }

func parseServerKey(raw []byte) (*ServerKey, error) {
	blk, _ := pem.Decode(raw)
	if blk == nil {
		return nil, errors.New("no PEM block")
	}
	var priv *rsa.PrivateKey
	if k, err := x509.ParsePKCS1PrivateKey(blk.Bytes); err == nil {
		priv = k
	} else if k8, err8 := x509.ParsePKCS8PrivateKey(blk.Bytes); err8 == nil {
		var ok bool
		if priv, ok = k8.(*rsa.PrivateKey); !ok {
			return nil, fmt.Errorf("PKCS#8 key is %T, RSA expected", k8)
		}
	} else {
		return nil, fmt.Errorf("not an RSA private key: %v", err)
	}
	if priv.N.BitLen() != 2048 {
		return nil, fmt.Errorf("RSA key of %d bits, 2048 expected", priv.N.BitLen())
	}
	if err := priv.Validate(); err != nil {
		return nil, err
	}
	priv.Precompute()
	return &ServerKey{priv: priv, fp: crypto.RSAFingerprint(&priv.PublicKey)}, nil
}

// Fingerprint is what clients pin.
func (k *ServerKey) Fingerprint() int64 { return k.fp }

// rawDecrypt is textbook RSA (m = c^d mod n) with blinding: math/big's Exp is not constant time, so the exponent works on c*r^e with a fresh
// random r and the result is unblinded with r^-1. Without blinding the decryption time would follow c and could leak d to a remote timer.
func (k *ServerKey) rawDecrypt(data []byte) ([]byte, error) {
	n := k.priv.N
	c := new(big.Int).SetBytes(data)
	if c.Sign() == 0 || c.Cmp(n) >= 0 {
		return nil, errors.New("RSA input is 0 or not below the modulus")
	}
	var r, rInv *big.Int
	for {
		var err error
		if r, err = rand.Int(rand.Reader, n); err != nil {
			return nil, err
		}
		if r.Sign() == 0 {
			continue
		}
		if rInv = new(big.Int).ModInverse(r, n); rInv != nil {
			break
		}
	}
	e := big.NewInt(int64(k.priv.E))
	cb := new(big.Int).Exp(r, e, n)
	cb.Mul(cb, c).Mod(cb, n)
	m := new(big.Int).Exp(cb, k.priv.D, n)
	m.Mul(m, rInv).Mod(m, n)
	out := make([]byte, 256)
	m.FillBytes(out)
	return out, nil
}

// decodeRSAPad is the server side of RSA_PAD (core.telegram.org/mtproto/auth_key, step 4): it returns data_with_padding (192 bytes,
// the inner data first). The hash check is constant time.
func (k *ServerKey) decodeRSAPad(encrypted []byte) ([]byte, error) {
	if len(encrypted) != 256 {
		return nil, fmt.Errorf("encrypted_data of %d bytes, 256 expected", len(encrypted))
	}
	keyAES, err := k.rawDecrypt(encrypted)
	if err != nil {
		return nil, err
	}
	tempKeyXor, aesEncrypted := keyAES[:32], keyAES[32:]
	h := sha256.Sum256(aesEncrypted)
	tempKey := make([]byte, 32)
	subtle.XORBytes(tempKey, tempKeyXor, h[:])
	blk, err := aes.NewCipher(tempKey)
	if err != nil {
		return nil, err
	}
	withHash := make([]byte, len(aesEncrypted))
	ige.DecryptBlocks(blk, make([]byte, 32), withHash, aesEncrypted)
	data := withHash[:192]
	for i, j := 0, len(data)-1; i < j; i, j = i+1, j-1 {
		data[i], data[j] = data[j], data[i]
	}
	want := sha256.Sum256(append(append([]byte(nil), tempKey...), data...))
	if subtle.ConstantTimeCompare(want[:], withHash[192:]) != 1 {
		return nil, errors.New("RSA_PAD hash does not match (encrypted for another key, or corrupted)")
	}
	return data, nil
}
