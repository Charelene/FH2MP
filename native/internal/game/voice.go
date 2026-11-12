package game

// Voice relay (RE 2026-11-12, Notion "Voice A0: Microsoft's GameChat engine, read for its wire format"; the client side is the Guide's
// ChatManager, guide/src/voice.cpp on feature/mtproto).
//
// The game sends unreliable chat packets after the messages of a datagram, each as `0xC4A7:16 | size:16 | payload` (HeaderToServer,
// game+0x176a860; the game's own voice datagrams carry no messages), and reads what the server relays as `0xC4A7:16 | size:16 | source
// console id:64 | payload` (HeaderFromServer, game+0x1767030; size below 0x55d, game+0x1763ab0). It never decodes a payload: the engines
// on both ends do. So the server copies each payload, unread, to the other entered players of the sender's world, stamped with the
// sender's xuid as its console id.

import (
	"encoding/binary"
	"fmt"

	"fh2mp/internal/bits"
	"fh2mp/internal/diag"
	"fh2mp/internal/netv2"
)

const (
	VoiceMagic       = 0xC4A7
	VoiceMaxPayload  = 512 // ours are about 60 bytes (two 40 ms Opus frames at 6 kbit/s); the game allows 0x55c
	VoicePerSecond   = 30  // packets a player may send each second (ours: 12.5)
	voiceBurstTokens = 30
)

// VoiceDatagram is one relayed chat packet for a receiver: a header with no messages, then HeaderFromServer and the payload. The header
// is 88 bits, so the tail starts on a byte and its fields are little-endian bytes, as the bit stream writes them.
func VoiceDatagram(ack uint16, ackBits uint32, from uint64, payload []byte) []byte {
	n := netv2.HeaderBits/8 + 12 + len(payload)
	out := make([]byte, n)
	binary.LittleEndian.PutUint16(out[0:], netv2.UnreliableID)
	binary.LittleEndian.PutUint16(out[2:], ack)
	binary.LittleEndian.PutUint32(out[4:], ackBits)
	out[8] = 0
	binary.LittleEndian.PutUint16(out[9:], uint16(n))
	binary.LittleEndian.PutUint16(out[11:], VoiceMagic)
	binary.LittleEndian.PutUint16(out[13:], uint16(len(payload)))
	binary.LittleEndian.PutUint64(out[15:], from)
	copy(out[23:], payload)
	return out
}

// readVoiceTails reads every chat packet left in a datagram after its messages. It stops at anything that is not one (the caller logs
// what is left). bad says why the last one was refused, if one was.
func readVoiceTails(br *bits.Reader) (payloads [][]byte, bad string) {
	for br.Size-br.Pos >= 32 {
		at := br.Pos
		magic, _ := br.Bits(16)
		if magic != VoiceMagic {
			br.Pos = at
			return payloads, ""
		}
		size, _ := br.Bits(16)
		if size == 0 || int(size) > VoiceMaxPayload {
			br.Pos = at
			return payloads, fmt.Sprintf("a chat packet of %d bytes (the limit is %d)", size, VoiceMaxPayload)
		}
		if int(size)*8 > br.Size-br.Pos {
			br.Pos = at
			return payloads, fmt.Sprintf("a chat packet of %d bytes with %d bits left", size, br.Size-br.Pos)
		}
		p := make([]byte, size)
		for i := range p {
			v, _ := br.Bits(8)
			p[i] = byte(v)
		}
		payloads = append(payloads, p)
	}
	return payloads, ""
}

// voiceAllowed takes one packet from the sender's budget (VoicePerSecond, bursts up to voiceBurstTokens).
func (p *Peer) voiceAllowed(now float64) bool {
	if p.VoiceAt == 0 {
		p.VoiceTokens, p.VoiceAt = voiceBurstTokens, now
	}
	p.VoiceTokens += (now - p.VoiceAt) * VoicePerSecond
	if p.VoiceTokens > voiceBurstTokens {
		p.VoiceTokens = voiceBurstTokens
	}
	p.VoiceAt = now
	if p.VoiceTokens < 1 {
		return false
	}
	p.VoiceTokens--
	return true
}

// relayVoice relays the chat packets at the end of a datagram from peer; it moves br past them.
func (w *World) relayVoice(peer *Peer, br *bits.Reader) {
	if br.Size-br.Pos < 32 {
		return
	}
	at := br.Pos
	if magic, _ := br.Bits(16); magic != VoiceMagic {
		br.Pos = at
		return
	}
	br.Pos = at
	payloads, bad := readVoiceTails(br)
	if bad != "" && !peer.VoiceBadLogged {
		peer.VoiceBadLogged = true
		w.Warnf("%d voice: %s refused, the rest of the datagram is not relayed (owner %d xuid %#x; once per player)", peer.Addr.Port, bad, peer.Owner, peer.Xuid)
	}
	if len(payloads) == 0 {
		return
	}
	if !peer.Connected || !peer.Entered || peer.Xuid == 0 {
		return
	}
	others := w.Others(peer)
	now := w.Mono()
	sent := 0
	for _, p := range payloads {
		if !peer.voiceAllowed(now) {
			peer.VoiceDropped++
			continue
		}
		for _, q := range others {
			ack, ackBits := q.AckFields()
			if err := w.Sock.SendTo(VoiceDatagram(uint16(ack), ackBits, peer.Xuid, p), q.Addr); err != nil {
				w.Noise(fmt.Sprintf("%d voice send failed: %v", q.Addr.Port, err), diag.Info)
			}
		}
		sent++
	}
	peer.VoicePackets += sent
	if !peer.VoiceLogged && sent > 0 {
		peer.VoiceLogged = true
		w.Logf("%d voice: first chat packet from owner %d xuid %#x (%d bytes), relayed to the %d other players of its world", peer.Addr.Port, peer.Owner, peer.Xuid, len(payloads[0]), len(others))
	}
}
