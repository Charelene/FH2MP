package mt

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"time"

	"github.com/gotd/td/mtproxy/obfuscated2"
)

// Transport tags (core.telegram.org/mtproto/mtproto-transports). We take intermediate and padded intermediate, bare or inside obfuscated2;
// abridged and full are refused: one framing to get right, and obfuscated2 already hides the tag from the network.
var (
	tagIntermediate = [4]byte{0xee, 0xee, 0xee, 0xee}
	tagPadded       = [4]byte{0xdd, 0xdd, 0xdd, 0xdd}
)

const obfHeaderLen = 64

// Transport error codes sent as a 4-byte frame holding the negative code (MTProto transport errors).
const (
	transportKeyNotFound = 404
	transportFlood       = 429
)

// errTransportRefused is a client whose first bytes are no transport we accept.
type errTransportRefused struct{ why string }

func (e errTransportRefused) Error() string { return "transport refused: " + e.why }

// frameConn is one client connection after its transport header: framing (padded or not) over the raw or the deobfuscated stream.
// Reads happen on the connection's own goroutine; writes may come from any goroutine (pushes) and are serialized by wmu.
type frameConn struct {
	nc       net.Conn
	r        io.Reader
	w        io.Writer
	padded   bool
	obf      bool
	maxFrame int
	wmu      sync.Mutex
	hdr      [4]byte
}

// kind names the transport for log lines.
func (c *frameConn) kind() string {
	k := "intermediate"
	if c.padded {
		k = "padded"
	}
	if c.obf {
		k = "obfuscated2+" + k
	}
	return k
}

// acceptTransport reads the transport header before the deadline. The first 4 bytes are a bare tag, or the start of a 64-byte obfuscated2 header
// whose decrypted bytes 56..60 carry the tag.
func acceptTransport(nc net.Conn, maxFrame int, deadline time.Time) (*frameConn, error) {
	if err := nc.SetReadDeadline(deadline); err != nil {
		return nil, err
	}
	var first [4]byte
	if _, err := io.ReadFull(nc, first[:]); err != nil {
		return nil, fmt.Errorf("transport header: %w", err)
	}
	c := &frameConn{nc: nc, r: nc, w: nc, maxFrame: maxFrame}
	switch {
	case first == tagIntermediate:
		return c, nil
	case first == tagPadded:
		c.padded = true
		return c, nil
	case first[0] == 0xef:
		return nil, errTransportRefused{"abridged framing (first byte 0xef); intermediate or padded intermediate expected"}
	}
	// obfuscated2: the 4 bytes read are the start of the 64-byte header.
	rd := io.MultiReader(bytes.NewReader(first[:]), nc)
	rw, md, err := obfuscated2.Accept(struct {
		io.Reader
		io.Writer
	}{rd, nc}, nil)
	if err != nil {
		return nil, fmt.Errorf("obfuscated2 header: %w", err)
	}
	switch md.Protocol {
	case tagIntermediate:
	case tagPadded:
		c.padded = true
	default:
		return nil, errTransportRefused{fmt.Sprintf("obfuscated2 header carries tag %x (not a known framing, or not obfuscated2 at all)", md.Protocol)}
	}
	c.r, c.w, c.obf = rw, rw, true
	return c, nil
}

// readFrame returns the next frame's payload. The quick-ack bit of the length is ignored (we send no quick acks).
func (c *frameConn) readFrame(deadline time.Time) ([]byte, error) {
	if err := c.nc.SetReadDeadline(deadline); err != nil {
		return nil, err
	}
	if _, err := io.ReadFull(c.r, c.hdr[:]); err != nil {
		return nil, err
	}
	n := int(binary.LittleEndian.Uint32(c.hdr[:]) & 0x7fffffff)
	if n < 4 || n > c.maxFrame {
		return nil, fmt.Errorf("frame length %d outside 4..%d", n, c.maxFrame)
	}
	if !c.padded && n%4 != 0 {
		return nil, fmt.Errorf("frame length %d is not a multiple of 4 (intermediate framing)", n)
	}
	b := make([]byte, n)
	if _, err := io.ReadFull(c.r, b); err != nil {
		return nil, fmt.Errorf("frame body (%d bytes): %w", n, err)
	}
	return b, nil
}

// writeFrame sends one frame; padded framing adds 0..15 random bytes, which the reader drops by the message's own length.
func (c *frameConn) writeFrame(payload []byte, deadline time.Time) error {
	pad := 0
	var rb [1]byte
	if c.padded {
		_, _ = rand.Read(rb[:])
		pad = int(rb[0] & 15)
	}
	buf := make([]byte, 4+len(payload)+pad)
	binary.LittleEndian.PutUint32(buf, uint32(len(payload)+pad))
	copy(buf[4:], payload)
	if pad > 0 {
		_, _ = rand.Read(buf[4+len(payload):])
	}
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if err := c.nc.SetWriteDeadline(deadline); err != nil {
		return err
	}
	_, err := c.w.Write(buf)
	return err
}

// writeTransportError sends the 4-byte transport error frame (-code).
func (c *frameConn) writeTransportError(code int32, deadline time.Time) error {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], uint32(-code))
	return c.writeFrame(b[:], deadline)
}

// trimEncrypted drops padded-framing bytes after an encrypted message: auth_key_id(8) + msg_key(16) + data(16*k).
func trimEncrypted(frame []byte, padded bool) ([]byte, error) {
	if len(frame) < 8+16+32 {
		return nil, fmt.Errorf("encrypted message of %d bytes, at least 56 expected", len(frame))
	}
	extra := (len(frame) - 24) % 16
	if extra != 0 {
		if !padded {
			return nil, fmt.Errorf("encrypted data of %d bytes is not a multiple of 16", len(frame)-24)
		}
		frame = frame[:len(frame)-extra]
	}
	return frame, nil
}

var errClosed = errors.New("connection closed")
