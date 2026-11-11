package mt

import (
	"bytes"
	"crypto/aes"
	"crypto/rand"
	"crypto/sha1"
	"crypto/subtle"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"math/big"
	"sync"
	"time"

	"github.com/gotd/ige"
	"github.com/gotd/td/bin"
	"github.com/gotd/td/crypto"
	"github.com/gotd/td/mt"
)

// The server side of the auth-key exchange (core.telegram.org/mtproto/auth_key). gotd's exchange.ServerExchange is not used: it hands out
// a fixed pq, does not compare the nonces of later steps with the first, does not range-check g_b, and decrypts RSA without blinding.
// Only temporary keys are made (p_q_inner_data_temp_dc): the server keeps them in memory and never stores them.

// dhPrime is the 2048-bit safe prime with g = 3 that Telegram's servers hand out (core.telegram.org/mtproto/auth_key, also in
// core.telegram.org/api/end-to-end); checked once with crypto.CheckDH before the first exchange.
var dhPrime = func() *big.Int {
	b, _ := hex.DecodeString("C71CAEB9C6B1C9048E6C522F70F13F73980D40238E3E21C14934D037563D930F" +
		"48198A0AA7C14058229493D22530F4DBFA336F6E0AC925139543AED44CCE7C37" +
		"20FD51F69458705AC68CD4FE6B6B13ABDC9746512969328454F18FAF8C595F64" +
		"2477FE96BB2A941D5BCD1D4AC8CC49880708FA9B378E3C4F3A9060BEE67CF9A4" +
		"A4A695811051907E162753B56B0F6B410DBA74D8A84B2A14B3144E0EF1284754" +
		"FD17ED950D5965B4B9DD46582DB1178D169C6BC465B0D6FF9CA3928FEF5B9AE4" +
		"E418FC15E83EBEA0F87FA9FF5EED70050DED2849F47BF959D956850CE929851F" +
		"0D8115F635B105EE2E4E15D04B2454BF6F4FADF034B10403119CD8E3B92FCC5B")
	return new(big.Int).SetBytes(b)
}()

const dhG = 3

var (
	dhCheckOnce sync.Once
	dhCheckErr  error
	// The safety range of g_a and g_b: 2^(2048-64) < g_x < dh_prime - 2^(2048-64).
	dhLow  = new(big.Int).Lsh(big.NewInt(1), 2048-64)
	dhHigh = new(big.Int).Sub(dhPrime, dhLow)
)

func checkDHPrime() error {
	dhCheckOnce.Do(func() { dhCheckErr = crypto.CheckDH(dhG, dhPrime) })
	return dhCheckErr
}

// exchangeError is a refused exchange: step names where, why is for the log line.
type exchangeError struct {
	step string
	why  string
}

func (e *exchangeError) Error() string { return e.step + ": " + e.why }

func xerr(step, format string, a ...any) error {
	return &exchangeError{step: step, why: fmt.Sprintf(format, a...)}
}

// exchangeResult is a made key: its value, the first server salt and how long the client asked it to live.
type exchangeResult struct {
	key       crypto.AuthKey
	salt      int64
	expiresIn int
	took      time.Duration
}

// exchangeConfig are the knobs of one exchange.
type exchangeConfig struct {
	key      *serverKey
	dc       int
	maxTTL   int
	minTTL   int
	stepWait time.Duration
	now      func() time.Time
	ids      *msgIDGen
	// keyExists says whether an auth_key_id is taken already (dh_gen_fail then; 2^-64 per key).
	keyExists func(id [8]byte) bool
}

// plainIO reads and writes the unencrypted messages of the exchange on one connection.
type plainIO struct {
	c   *frameConn
	cfg *exchangeConfig
}

func (p plainIO) read(step string) (*bin.Buffer, error) {
	frame, err := p.c.readFrame(p.cfg.now().Add(p.cfg.stepWait))
	if err != nil {
		return nil, xerr(step, "read: %v", err)
	}
	// auth_key_id(8) = 0, message_id(8), message_data_length(4), message_data
	if len(frame) < 20 {
		return nil, xerr(step, "unencrypted message of %d bytes, at least 20 expected", len(frame))
	}
	if binary.LittleEndian.Uint64(frame) != 0 {
		return nil, xerr(step, "an encrypted message arrived during the key exchange")
	}
	msgID := int64(binary.LittleEndian.Uint64(frame[8:]))
	if msgID%4 != 0 {
		return nil, xerr(step, "client message id %d is not a multiple of 4", msgID)
	}
	n := int(binary.LittleEndian.Uint32(frame[16:]))
	if n < 4 || n > len(frame)-20 {
		return nil, xerr(step, "message_data_length %d outside 4..%d", n, len(frame)-20)
	}
	return &bin.Buffer{Buf: frame[20 : 20+n]}, nil
}

func (p plainIO) write(step string, m bin.Encoder) error {
	var body bin.Buffer
	if err := m.Encode(&body); err != nil {
		return xerr(step, "encode: %v", err)
	}
	out := make([]byte, 20, 20+body.Len())
	binary.LittleEndian.PutUint64(out[8:], uint64(p.cfg.ids.next(true)))
	binary.LittleEndian.PutUint32(out[16:], uint32(body.Len()))
	out = append(out, body.Buf...)
	if err := p.c.writeFrame(out, p.cfg.now().Add(p.cfg.stepWait)); err != nil {
		return xerr(step, "write: %v", err)
	}
	return nil
}

// randomPQ makes pq = p*q with p < q two random 31-bit primes (pq < 2^62), the client's small proof of work.
func randomPQ() (pq, p, q *big.Int, err error) {
	for {
		if p, err = rand.Prime(rand.Reader, 31); err != nil {
			return
		}
		if q, err = rand.Prime(rand.Reader, 31); err != nil {
			return
		}
		switch p.Cmp(q) {
		case 0:
			continue
		case 1:
			p, q = q, p
		}
		return new(big.Int).Mul(p, q), p, q, nil
	}
}

// tmpAESKeys: tmp_aes_key = SHA1(new_nonce + server_nonce) + SHA1(server_nonce + new_nonce)[0:12];
// tmp_aes_iv = SHA1(server_nonce + new_nonce)[12:20] + SHA1(new_nonce + new_nonce) + new_nonce[0:4]. Nonces as on the wire.
func tmpAESKeys(newNonce bin.Int256, serverNonce bin.Int128) (key, iv []byte) {
	h := func(a, b []byte) []byte { s := sha1.Sum(append(append([]byte(nil), a...), b...)); return s[:] }
	ns, sn := newNonce[:], serverNonce[:]
	key = append(h(ns, sn), h(sn, ns)[:12]...)
	iv = append(append(append([]byte(nil), h(sn, ns)[12:20]...), h(ns, ns)...), ns[:4]...)
	return key, iv
}

// serverSalt is new_nonce[0:8] XOR server_nonce[0:8], read little-endian.
func serverSalt(newNonce bin.Int256, serverNonce bin.Int128) int64 {
	var b [8]byte
	subtle.XORBytes(b[:], newNonce[:8], serverNonce[:8])
	return int64(binary.LittleEndian.Uint64(b[:]))
}

// runExchange runs the three steps after the first frame (req_pq_multi) was read as first.
func runExchange(c *frameConn, first []byte, cfg *exchangeConfig) (*exchangeResult, error) {
	start := cfg.now()
	if err := checkDHPrime(); err != nil {
		return nil, xerr("setup", "dh_prime check failed: %v", err)
	}
	pio := plainIO{c: c, cfg: cfg}
	// step 1: req_pq_multi#be7e8ef1 nonce:int128 = ResPQ
	b, err := bufFromFirst(first)
	if err != nil {
		return nil, xerr("req_pq_multi", "%v", err)
	}
	var req mt.ReqPqMultiRequest
	if err := req.Decode(b); err != nil {
		return nil, xerr("req_pq_multi", "decode: %v (only req_pq_multi is taken, not the old req_pq)", err)
	}
	var serverNonce bin.Int128
	if _, err := rand.Read(serverNonce[:]); err != nil {
		return nil, err
	}
	pq, p, q, err := randomPQ()
	if err != nil {
		return nil, err
	}
	if err := pio.write("res_pq", &mt.ResPQ{Nonce: req.Nonce, ServerNonce: serverNonce, Pq: pq.Bytes(), ServerPublicKeyFingerprints: []int64{cfg.key.fp}}); err != nil {
		return nil, err
	}

	// step 2: req_DH_params nonce server_nonce p q public_key_fingerprint encrypted_data
	if b, err = pio.read("req_dh_params"); err != nil {
		return nil, err
	}
	var dh mt.ReqDHParamsRequest
	if err := dh.Decode(b); err != nil {
		return nil, xerr("req_dh_params", "decode: %v", err)
	}
	if dh.Nonce != req.Nonce || dh.ServerNonce != serverNonce {
		return nil, xerr("req_dh_params", "nonce or server_nonce differs from step 1")
	}
	if dh.PublicKeyFingerprint != cfg.key.fp {
		return nil, xerr("req_dh_params", "public key fingerprint %016x, ours is %016x (the client pins another server key)", uint64(dh.PublicKeyFingerprint), uint64(cfg.key.fp))
	}
	if !bytes.Equal(dh.P, p.Bytes()) || !bytes.Equal(dh.Q, q.Bytes()) {
		return nil, xerr("req_dh_params", "p and q are not the factors of pq (p < q, big-endian)")
	}
	inner, err := cfg.key.decodeRSAPad(dh.EncryptedData)
	if err != nil {
		return nil, xerr("req_dh_params", "encrypted_data: %v", err)
	}
	ib := &bin.Buffer{Buf: inner}
	id, _ := ib.PeekID()
	if id != mt.PQInnerDataTempDCTypeID {
		return nil, xerr("req_dh_params", "inner data %#08x, p_q_inner_data_temp_dc expected (this server makes temporary keys only)", id)
	}
	var in mt.PQInnerDataTempDC
	if err := in.Decode(ib); err != nil {
		return nil, xerr("req_dh_params", "inner data decode: %v", err)
	}
	if in.Nonce != req.Nonce || in.ServerNonce != serverNonce || !bytes.Equal(in.Pq, pq.Bytes()) || !bytes.Equal(in.P, dh.P) || !bytes.Equal(in.Q, dh.Q) {
		return nil, xerr("req_dh_params", "inner data does not repeat the nonces, pq, p and q of the outer request")
	}
	if in.DC != cfg.dc {
		return nil, xerr("req_dh_params", "inner data names dc %d, this server is dc %d", in.DC, cfg.dc)
	}
	if in.ExpiresIn < cfg.minTTL {
		return nil, xerr("req_dh_params", "expires_in %d s, at least %d s expected", in.ExpiresIn, cfg.minTTL)
	}
	ttl := in.ExpiresIn
	if ttl > cfg.maxTTL {
		ttl = cfg.maxTTL
	}

	// step 3: server_DH_params_ok with server_DH_inner_data encrypted under the temporary AES key
	a, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 2048))
	if err != nil {
		return nil, err
	}
	ga := new(big.Int).Exp(big.NewInt(dhG), a, dhPrime)
	for ga.Cmp(dhLow) <= 0 || ga.Cmp(dhHigh) >= 0 {
		if a, err = rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 2048)); err != nil {
			return nil, err
		}
		ga.Exp(big.NewInt(dhG), a, dhPrime)
	}
	var ans bin.Buffer
	if err := (&mt.ServerDHInnerData{Nonce: req.Nonce, ServerNonce: serverNonce, G: dhG, DhPrime: dhPrime.Bytes(), GA: ga.Bytes(),
		ServerTime: int(cfg.now().Unix())}).Encode(&ans); err != nil {
		return nil, err
	}
	akey, aiv := tmpAESKeys(in.NewNonce, serverNonce)
	enc, err := encryptAnswer(ans.Buf, akey, aiv)
	if err != nil {
		return nil, err
	}
	if err := pio.write("server_dh_params", &mt.ServerDHParamsOk{Nonce: req.Nonce, ServerNonce: serverNonce, EncryptedAnswer: enc}); err != nil {
		return nil, err
	}

	// step 4: set_client_DH_params nonce server_nonce encrypted_data(client_DH_inner_data)
	if b, err = pio.read("set_client_dh_params"); err != nil {
		return nil, err
	}
	var set mt.SetClientDHParamsRequest
	if err := set.Decode(b); err != nil {
		return nil, xerr("set_client_dh_params", "decode: %v", err)
	}
	if set.Nonce != req.Nonce || set.ServerNonce != serverNonce {
		return nil, xerr("set_client_dh_params", "nonce or server_nonce differs from step 1")
	}
	var cin mt.ClientDHInnerData
	if err := decryptAnswer(set.EncryptedData, akey, aiv, &cin); err != nil {
		return nil, xerr("set_client_dh_params", "%v", err)
	}
	if cin.Nonce != req.Nonce || cin.ServerNonce != serverNonce {
		return nil, xerr("set_client_dh_params", "inner nonce or server_nonce differs from step 1")
	}
	if cin.RetryID != 0 {
		return nil, xerr("set_client_dh_params", "retry_id %d, 0 expected (no retry was asked for)", cin.RetryID)
	}
	gb := new(big.Int).SetBytes(cin.GB)
	if gb.Cmp(dhLow) <= 0 || gb.Cmp(dhHigh) >= 0 {
		return nil, xerr("set_client_dh_params", "g_b outside 2^1984 .. dh_prime - 2^1984")
	}
	var k crypto.Key
	new(big.Int).Exp(gb, a, dhPrime).FillBytes(k[:])
	authKey := k.WithID()
	if cfg.keyExists(authKey.ID) {
		_ = pio.write("dh_gen", &mt.DhGenFail{Nonce: req.Nonce, ServerNonce: serverNonce, NewNonceHash3: newNonceHash(in.NewNonce, k, 3)})
		return nil, xerr("dh_gen", "auth_key_id %x is taken already", authKey.ID)
	}
	if err := pio.write("dh_gen", &mt.DhGenOk{Nonce: req.Nonce, ServerNonce: serverNonce, NewNonceHash1: newNonceHash(in.NewNonce, k, 1)}); err != nil {
		return nil, err
	}
	return &exchangeResult{key: authKey, salt: serverSalt(in.NewNonce, serverNonce), expiresIn: ttl, took: cfg.now().Sub(start)}, nil
}

func bufFromFirst(frame []byte) (*bin.Buffer, error) {
	if len(frame) < 20 {
		return nil, fmt.Errorf("unencrypted message of %d bytes, at least 20 expected", len(frame))
	}
	if int64(binary.LittleEndian.Uint64(frame[8:]))%4 != 0 {
		return nil, errors.New("client message id is not a multiple of 4")
	}
	n := int(binary.LittleEndian.Uint32(frame[16:]))
	if n < 4 || n > len(frame)-20 {
		return nil, fmt.Errorf("message_data_length %d outside 4..%d", n, len(frame)-20)
	}
	return &bin.Buffer{Buf: frame[20 : 20+n]}, nil
}

// newNonceHash is the last 128 bits of SHA1(new_nonce + n + SHA1(auth_key)[0:8]) (n = 1 ok, 2 retry, 3 fail).
func newNonceHash(newNonce bin.Int256, k crypto.Key, n byte) (r bin.Int128) {
	aux := sha1.Sum(k[:])
	s := sha1.Sum(append(append(append([]byte(nil), newNonce[:]...), n), aux[:8]...))
	copy(r[:], s[4:20])
	return r
}

// encryptAnswer: AES-256-IGE over SHA1(answer) + answer + random padding to a multiple of 16.
func encryptAnswer(answer, key, iv []byte) ([]byte, error) {
	h := sha1.Sum(answer)
	n := 20 + len(answer)
	padded := make([]byte, (n+15)/16*16)
	copy(padded, h[:])
	copy(padded[20:], answer)
	if _, err := rand.Read(padded[n:]); err != nil {
		return nil, err
	}
	blk, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	out := make([]byte, len(padded))
	ige.EncryptBlocks(blk, iv, out, padded)
	return out, nil
}

// decryptAnswer decrypts data_with_hash and decodes into m; the SHA1 must cover exactly the decoded bytes, with at most 15 bytes of padding after.
func decryptAnswer(data, key, iv []byte, m bin.Decoder) error {
	if len(data) < 32 || len(data)%16 != 0 {
		return fmt.Errorf("encrypted_data of %d bytes, a multiple of 16 and at least 32 expected", len(data))
	}
	blk, err := aes.NewCipher(key)
	if err != nil {
		return err
	}
	plain := make([]byte, len(data))
	ige.DecryptBlocks(blk, iv, plain, data)
	b := &bin.Buffer{Buf: plain[20:]}
	if err := m.Decode(b); err != nil {
		return fmt.Errorf("inner data decode: %v", err)
	}
	used := len(plain) - 20 - b.Len()
	if b.Len() > 15 {
		return fmt.Errorf("%d bytes after the inner data, at most 15 of padding expected", b.Len())
	}
	h := sha1.Sum(plain[20 : 20+used])
	if subtle.ConstantTimeCompare(h[:], plain[:20]) != 1 {
		return errors.New("SHA1 of the inner data does not match (wrong temporary key: new_nonce differs)")
	}
	return nil
}
