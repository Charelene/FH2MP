package mt

// End-to-end direct messages (board item "MTProto chat P3"; the P3 contract, sections 2.1-2.8, after Telegram's secret chats,
// core.telegram.org/api/end-to-end): chat devices, secret chats, the relay of sealed messages and franking reports. The server sees who talks
// to whom on which device, the public halves of each key exchange with their device signatures, and the sealed bytes until the recipient
// has them. It never sees a chat key, a secret exponent, a plaintext or a device's private key, and its log lines name chat ids, accounts
// and sizes only.

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"fmt"
	"sync"
	"time"

	"github.com/gotd/td/bin"

	"fh2mp/internal/diag"
	"fh2mp/internal/mt/e2e"
	"fh2mp/internal/mt/tl"
)

const (
	maxReportReason = 200
	maxDhRandom     = 256
)

// SetChatStore plugs the end-to-end store in; without it those methods answer 501 NOT_IMPLEMENTED.
func (s *Server) SetChatStore(cs ChatStore) { s.chats = cs }

var errNoChats = &RPCError{Code: 501, Message: "NOT_IMPLEMENTED"}

// party is the caller of an end-to-end function: the account, the session's device id (auth.signIn) and the account's public key.
type party struct {
	xuid, device int64
	accountKey   []byte
}

func (c *Call) party() party {
	c.k.mu.Lock()
	defer c.k.mu.Unlock()
	return party{xuid: c.k.xuid, device: c.k.device, accountKey: c.k.accountKey}
}

func hexID(x int64) string { return fmt.Sprintf("%#x", x) }
func devID(d int64) string { return fmt.Sprintf("%016x", uint64(d)) }

func (s *Server) e2eInternal(p party, method string, err error) *RPCError {
	log.Exc("mt.call_failed "+diag.KV(diag.KVP{K: "method", V: method}, diag.KVP{K: "xuid", V: hexID(p.xuid)}), err)
	return &RPCError{Code: 500, Message: "INTERNAL"}
}

func (s *Server) areFriends(a, b int64) (bool, error) {
	if s.community == nil {
		return false, nil
	}
	return s.community.AreFriends(a, b)
}

func deviceTL(d *ChatDevice) *tl.ChatDevice {
	return &tl.ChatDevice{UserID: d.User, DeviceID: d.Device, Pubkey: d.Pubkey, AccountKey: d.AccountKey, Signature: d.Signature, Date: d.Date}
}

func (c *SecretChat) ends() e2e.Ends {
	return e2e.Ends{Admin: c.Admin, AdminDevice: c.AdminDevice, Participant: c.Participant, ParticipantDevice: c.ParticipantDevice}
}

// adminView is the chat as its admin sees it: waiting, accepted with the participant's g_b, or discarded.
func (c *SecretChat) adminView() tl.EncryptedChatClass {
	switch c.State {
	case ChatRequested:
		return &tl.EncryptedChatWaiting{ID: int(c.ID), Date: c.Date, AdminID: c.Admin, AdminDevice: c.AdminDevice, ParticipantID: c.Participant,
			ParticipantDevice: c.ParticipantDevice}
	case ChatAccepted:
		return &tl.EncryptedChat{ID: int(c.ID), Date: c.Date, AdminID: c.Admin, AdminDevice: c.AdminDevice, ParticipantID: c.Participant,
			ParticipantDevice: c.ParticipantDevice, GAOrB: c.GB, Sig: c.GBSig, KeyFingerprint: c.Fingerprint}
	}
	return &tl.EncryptedChatDiscarded{ID: int(c.ID)}
}

// storeChatUpdate stores an update of a secret chat for x (with pts) and pushes it.
func (s *Server) storeChatUpdate(x int64, chat int32, u tl.UpdateClass, message bool) error {
	var b bin.Buffer
	if err := u.Encode(&b); err != nil {
		return err
	}
	pts, err := s.store.AppendChat(x, b.Buf, chat, message)
	if err != nil {
		return err
	}
	s.Push(x, []tl.UpdateClass{u}, pts)
	return nil
}

// discardLocked discards a chat that is not discarded yet: both ends get updateEncryption(encryptedChatDiscarded) and the sealed messages
// still waiting on it are dropped. The caller holds e2eMu.
func (s *Server) discardLocked(c SecretChat, why string) {
	now := s.cfg.Now()
	ok, err := s.chats.Discard(c.ID, now)
	if err != nil {
		log.Exc("mt.e2e_discard_failed "+diag.KV(diag.KVP{K: "chat", V: c.ID}), err)
		return
	}
	if !ok {
		return
	}
	dropped, err := s.store.DropChat(c.ID)
	if err != nil {
		log.Exc("mt.e2e_discard_failed "+diag.KV(diag.KVP{K: "chat", V: c.ID})+": the chat is discarded, its waiting messages were not dropped", err, diag.Warn)
	}
	u := &tl.UpdateEncryption{Chat: &tl.EncryptedChatDiscarded{ID: int(c.ID)}, Date: int(now.Unix())}
	for _, x := range []int64{c.Admin, c.Participant} {
		if err := s.storeChatUpdate(x, c.ID, u, false); err != nil {
			log.Exc("mt.e2e_discard_failed "+diag.KV(diag.KVP{K: "chat", V: c.ID}, diag.KVP{K: "xuid", V: hexID(x)})+": the discard was not stored for this end", err)
		}
	}
	log.Info("mt.e2e_chat_discarded " + diag.KV(diag.KVP{K: "chat", V: c.ID}, diag.KVP{K: "admin", V: hexID(c.Admin)}, diag.KVP{K: "participant", V: hexID(c.Participant)},
		diag.KVP{K: "was", V: c.State.String()}, diag.KVP{K: "dropped_messages", V: nz(int(dropped))}) + ": " + why)
}

func (s *Server) registerE2E() {
	d := s.disp
	need := func(ctx context.Context) (party, error) {
		c := callOf(ctx)
		if s.chats == nil {
			return party{}, errNoChats
		}
		return c.party(), nil
	}
	d.OnAccountRegisterDevice(func(ctx context.Context, r *tl.AccountRegisterDeviceRequest) (*tl.ChatDevice, error) {
		p, err := need(ctx)
		if err != nil {
			return nil, err
		}
		return s.e2eRegisterDevice(p, r.Pubkey, r.Signature)
	})
	d.OnUsersGetDevices(func(ctx context.Context, user int64) (*tl.UsersDevices, error) {
		p, err := need(ctx)
		if err != nil {
			return nil, err
		}
		return s.e2eDevices(p, user)
	})
	d.OnMessagesGetDhConfig(func(ctx context.Context, r *tl.MessagesGetDhConfigRequest) (tl.MessagesDhConfigClass, error) {
		if _, err := need(ctx); err != nil {
			return nil, err
		}
		return s.e2eDhConfig(r.Version, r.RandomLength)
	})
	d.OnMessagesRequestEncryption(func(ctx context.Context, r *tl.MessagesRequestEncryptionRequest) (tl.EncryptedChatClass, error) {
		p, err := need(ctx)
		if err != nil {
			return nil, err
		}
		return s.e2eRequest(p, r)
	})
	d.OnMessagesAcceptEncryption(func(ctx context.Context, r *tl.MessagesAcceptEncryptionRequest) (tl.EncryptedChatClass, error) {
		p, err := need(ctx)
		if err != nil {
			return nil, err
		}
		return s.e2eAccept(p, r)
	})
	d.OnMessagesDiscardEncryption(func(ctx context.Context, chat int) (bool, error) {
		p, err := need(ctx)
		if err != nil {
			return false, err
		}
		if err := s.e2eDiscard(p, chat); err != nil {
			return false, err
		}
		return true, nil
	})
	d.OnMessagesSendEncrypted(func(ctx context.Context, r *tl.MessagesSendEncryptedRequest) (*tl.MessagesSentEncryptedMessage, error) {
		p, err := need(ctx)
		if err != nil {
			return nil, err
		}
		return s.e2eSend(p, r)
	})
	d.OnMessagesAckEncrypted(func(ctx context.Context, maxPts int) (bool, error) {
		p, err := need(ctx)
		if err != nil {
			return false, err
		}
		if err := s.e2eAck(p, maxPts); err != nil {
			return false, err
		}
		return true, nil
	})
	d.OnMessagesReportEncrypted(func(ctx context.Context, r *tl.MessagesReportEncryptedRequest) (bool, error) {
		p, err := need(ctx)
		if err != nil {
			return false, err
		}
		if err := s.e2eReport(p, r); err != nil {
			return false, err
		}
		return true, nil
	})
}

// e2eRegisterDevice: account.registerDevice (contract 2.1).
func (s *Server) e2eRegisterDevice(p party, pub, sig []byte) (*tl.ChatDevice, error) {
	const method = "account.registerDevice"
	if !e2e.VerifyDevice(p.accountKey, s.cfg.Realm, p.device, pub, sig) {
		return nil, rpcErr(400, "DEVICE_SIGNATURE_INVALID", fmt.Sprintf("the certificate of a %d-byte key (%d-byte signature) for device %s does not verify "+
			"against the account key in realm %s", len(pub), len(sig), devID(p.device), s.cfg.Realm))
	}
	s.e2eMu.Lock()
	defer s.e2eMu.Unlock()
	cur, err := s.chats.Device(p.xuid)
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if cur != nil && cur.Device == p.device && bytes.Equal(cur.Pubkey, pub) {
		return deviceTL(cur), nil
	}
	d := ChatDevice{User: p.xuid, Device: p.device, Pubkey: pub, AccountKey: p.accountKey, Signature: sig, Date: int(s.cfg.Now().Unix())}
	old, err := s.chats.SetDevice(d)
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if old == nil {
		log.Info("mt.e2e_device_registered " + diag.KV(diag.KVP{K: "xuid", V: hexID(p.xuid)}, diag.KVP{K: "device", V: devID(p.device)}))
		return deviceTL(&d), nil
	}
	// A new device: the chats of the old one cannot go on (their keys are on the other PC), friends re-pin.
	chats, err := s.chats.OpenChats(p.xuid, 0)
	if err != nil {
		log.Exc("mt.e2e_device_replaced "+diag.KV(diag.KVP{K: "xuid", V: hexID(p.xuid)})+": the account's chats were not read, none discarded", err)
	}
	for _, c := range chats {
		s.discardLocked(c, "the account registered a new chat device")
	}
	var friends []int64
	if s.community != nil {
		if friends, err = s.community.FriendIDs(p.xuid); err != nil {
			log.Exc("mt.e2e_device_replaced "+diag.KV(diag.KVP{K: "xuid", V: hexID(p.xuid)})+": friends not read, updateChatDevice not pushed", err, diag.Warn)
		}
	}
	s.PushShort(friends, &tl.UpdateChatDevice{UserID: p.xuid, DeviceID: p.device})
	log.Info("mt.e2e_device_replaced " + diag.KV(diag.KVP{K: "xuid", V: hexID(p.xuid)}, diag.KVP{K: "old_device", V: devID(old.Device)},
		diag.KVP{K: "new_device", V: devID(p.device)}, diag.KVP{K: "key_changed", V: !bytes.Equal(old.Pubkey, pub)}, diag.KVP{K: "chats_discarded", V: len(chats)},
		diag.KVP{K: "friends_told", V: len(friends)}))
	return deviceTL(&d), nil
}

// e2eDevices: users.getDevices, for the account itself or a friend.
func (s *Server) e2eDevices(p party, user int64) (*tl.UsersDevices, error) {
	if user != p.xuid {
		ok, err := s.areFriends(p.xuid, user)
		if err != nil {
			return nil, s.e2eInternal(p, "users.getDevices", err)
		}
		if !ok {
			return nil, rpcErr(403, "USER_NOT_FRIEND", fmt.Sprintf("devices of %s asked, not a friend", hexID(user)))
		}
	}
	d, err := s.chats.Device(user)
	if err != nil {
		return nil, s.e2eInternal(p, "users.getDevices", err)
	}
	out := &tl.UsersDevices{Devices: []tl.ChatDevice{}}
	if d != nil {
		out.Devices = append(out.Devices, *deviceTL(d))
	}
	return out, nil
}

// e2eDhConfig: messages.getDhConfig (contract 2.2).
func (s *Server) e2eDhConfig(version, n int) (tl.MessagesDhConfigClass, error) {
	if n < 0 || n > maxDhRandom {
		return nil, rpcErr(400, "RANDOM_LENGTH_INVALID", fmt.Sprintf("random_length %d, 0..%d", n, maxDhRandom))
	}
	random := make([]byte, n)
	if _, err := rand.Read(random); err != nil {
		return nil, err
	}
	if version == e2e.GroupVersion {
		return &tl.MessagesDhConfigNotModified{Random: random}, nil
	}
	return &tl.MessagesDhConfig{G: e2e.G, P: e2e.PBytes(), Version: e2e.GroupVersion, Random: random}, nil
}

// e2eRequest: messages.requestEncryption (contract 2.3).
func (s *Server) e2eRequest(p party, r *tl.MessagesRequestEncryptionRequest) (tl.EncryptedChatClass, error) {
	const method = "messages.requestEncryption"
	if r.UserID == p.xuid {
		return nil, rpcErr(403, "USER_NOT_FRIEND", "a secret chat with oneself")
	}
	s.e2eMu.Lock()
	defer s.e2eMu.Unlock()
	mine, err := s.chats.Device(p.xuid)
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if mine == nil {
		return nil, rpcErr(400, "DEVICE_NOT_REGISTERED", "the account has no chat device")
	}
	if mine.Device != p.device {
		return nil, rpcErr(400, "DEVICE_NOT_REGISTERED", fmt.Sprintf("the account's chat device is %s, this session signed in as %s", devID(mine.Device), devID(p.device)))
	}
	friends, err := s.areFriends(p.xuid, r.UserID)
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if !friends {
		return nil, rpcErr(403, "USER_NOT_FRIEND", fmt.Sprintf("%s is not a friend", hexID(r.UserID)))
	}
	theirs, err := s.chats.Device(r.UserID)
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if theirs == nil || theirs.Device != r.DeviceID {
		why := fmt.Sprintf("%s has no chat device", hexID(r.UserID))
		if theirs != nil {
			why = fmt.Sprintf("%s's chat device is %s, the request names %s", hexID(r.UserID), devID(theirs.Device), devID(r.DeviceID))
		}
		return nil, rpcErr(400, "DEVICE_CHANGED", why)
	}
	if err := e2e.CheckPublic(r.GA); err != nil {
		return nil, rpcErr(400, "DH_G_A_INVALID", err.Error())
	}
	ends := e2e.Ends{Admin: p.xuid, AdminDevice: p.device, Participant: r.UserID, ParticipantDevice: r.DeviceID}
	if !e2e.VerifyRequest(mine.Pubkey, ends, r.GA, r.GASig) {
		return nil, rpcErr(400, "DH_SIGNATURE_INVALID", fmt.Sprintf("g_a_sig (%d bytes) does not verify against device %s's key", len(r.GASig), devID(p.device)))
	}
	now := s.cfg.Now()
	c, made, err := s.chats.NewChat(SecretChat{Date: int(now.Unix()), Admin: p.xuid, AdminDevice: p.device, Participant: r.UserID,
		ParticipantDevice: r.DeviceID, RandomID: int32(r.RandomID), GA: r.GA, GASig: r.GASig}, now)
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if made {
		u := &tl.UpdateEncryption{Chat: &tl.EncryptedChatRequested{ID: int(c.ID), Date: c.Date, AdminID: c.Admin, AdminDevice: c.AdminDevice,
			ParticipantID: c.Participant, ParticipantDevice: c.ParticipantDevice, GA: c.GA, GASig: c.GASig}, Date: int(now.Unix())}
		if err := s.storeChatUpdate(c.Participant, c.ID, u, false); err != nil {
			// The participant would never hear of the chat: it goes, and a repeat of the request makes a new one.
			_, _ = s.chats.Discard(c.ID, now)
			return nil, s.e2eInternal(p, method, err)
		}
		log.Info("mt.e2e_chat_requested " + diag.KV(diag.KVP{K: "chat", V: c.ID}, diag.KVP{K: "admin", V: hexID(c.Admin)}, diag.KVP{K: "admin_device", V: devID(c.AdminDevice)},
			diag.KVP{K: "participant", V: hexID(c.Participant)}, diag.KVP{K: "participant_device", V: devID(c.ParticipantDevice)}))
	}
	return c.adminView(), nil
}

// e2eAccept: messages.acceptEncryption (contract 2.3).
func (s *Server) e2eAccept(p party, r *tl.MessagesAcceptEncryptionRequest) (tl.EncryptedChatClass, error) {
	const method = "messages.acceptEncryption"
	s.e2eMu.Lock()
	defer s.e2eMu.Unlock()
	c, err := s.chats.Chat(int32(r.ChatID))
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if c == nil || c.Participant != p.xuid {
		why := fmt.Sprintf("no chat %d", r.ChatID)
		if c != nil {
			why = fmt.Sprintf("chat %d is not the caller's to accept", r.ChatID)
		}
		return nil, rpcErr(400, "ENCRYPTION_ID_INVALID", why)
	}
	switch c.State {
	case ChatDiscarded:
		return nil, rpcErr(400, "ENCRYPTION_DECLINED", fmt.Sprintf("chat %d was discarded", c.ID))
	case ChatAccepted:
		return nil, rpcErr(400, "ENCRYPTION_ALREADY_ACCEPTED", fmt.Sprintf("chat %d", c.ID))
	}
	if err := e2e.CheckPublic(r.GB); err != nil {
		return nil, rpcErr(400, "DH_G_B_INVALID", err.Error())
	}
	dev, err := s.chats.Device(p.xuid)
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if dev == nil || dev.Device != c.ParticipantDevice {
		return nil, rpcErr(400, "ENCRYPTION_DECLINED", fmt.Sprintf("chat %d was made for device %s, which is not the account's chat device", c.ID, devID(c.ParticipantDevice)))
	}
	if !e2e.VerifyAccept(dev.Pubkey, c.ID, c.ends(), c.GA, r.GB, r.KeyFingerprint, r.GBSig) {
		return nil, rpcErr(400, "DH_SIGNATURE_INVALID", fmt.Sprintf("g_b_sig (%d bytes) does not verify against device %s's key", len(r.GBSig), devID(dev.Device)))
	}
	now := s.cfg.Now()
	ok, err := s.chats.Accept(c.ID, r.GB, r.GBSig, r.KeyFingerprint, now)
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if !ok {
		return nil, rpcErr(400, "ENCRYPTION_ALREADY_ACCEPTED", fmt.Sprintf("chat %d left the requested state meanwhile", c.ID))
	}
	u := &tl.UpdateEncryption{Chat: &tl.EncryptedChat{ID: int(c.ID), Date: c.Date, AdminID: c.Admin, AdminDevice: c.AdminDevice, ParticipantID: c.Participant,
		ParticipantDevice: c.ParticipantDevice, GAOrB: r.GB, Sig: r.GBSig, KeyFingerprint: r.KeyFingerprint}, Date: int(now.Unix())}
	if err := s.storeChatUpdate(c.Admin, c.ID, u, false); err != nil {
		log.Exc("mt.e2e_accept_failed "+diag.KV(diag.KVP{K: "chat", V: c.ID})+": accepted but the admin's update was not stored; the chat is discarded", err)
		s.discardLocked(*c, "the accept could not be handed to the admin")
		return nil, s.e2eInternal(p, method, err)
	}
	log.Info("mt.e2e_chat_accepted " + diag.KV(diag.KVP{K: "chat", V: c.ID}, diag.KVP{K: "admin", V: hexID(c.Admin)}, diag.KVP{K: "participant", V: hexID(c.Participant)}))
	return &tl.EncryptedChat{ID: int(c.ID), Date: c.Date, AdminID: c.Admin, AdminDevice: c.AdminDevice, ParticipantID: c.Participant,
		ParticipantDevice: c.ParticipantDevice, GAOrB: c.GA, Sig: c.GASig, KeyFingerprint: r.KeyFingerprint}, nil
}

// e2eDiscard: messages.discardEncryption, by either end; discarding a discarded chat changes nothing.
func (s *Server) e2eDiscard(p party, chat int) error {
	s.e2eMu.Lock()
	defer s.e2eMu.Unlock()
	c, err := s.chats.Chat(int32(chat))
	if err != nil {
		return s.e2eInternal(p, "messages.discardEncryption", err)
	}
	if c == nil || !c.Has(p.xuid) {
		return rpcErr(400, "ENCRYPTION_ID_INVALID", fmt.Sprintf("no chat %d of the caller's", chat))
	}
	if c.State != ChatDiscarded {
		s.discardLocked(*c, "discarded by "+hexID(p.xuid))
	}
	return nil
}

// e2eSend: messages.sendEncrypted (contract 2.8): the sealed bytes become the other end's stored update.
func (s *Server) e2eSend(p party, r *tl.MessagesSendEncryptedRequest) (*tl.MessagesSentEncryptedMessage, error) {
	const method = "messages.sendEncrypted"
	s.e2eMu.Lock()
	defer s.e2eMu.Unlock()
	c, err := s.chats.Chat(int32(r.ChatID))
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if c == nil || !c.Has(p.xuid) || c.State != ChatAccepted {
		why := fmt.Sprintf("no chat %d of the caller's", r.ChatID)
		if c != nil && c.Has(p.xuid) {
			why = fmt.Sprintf("chat %d is %s", c.ID, c.State)
		}
		return nil, rpcErr(400, "ENCRYPTION_ID_INVALID", why)
	}
	if len(r.Data) > e2e.MaxData {
		return nil, rpcErr(400, "MESSAGE_TOO_LONG", fmt.Sprintf("%d bytes, at most %d", len(r.Data), e2e.MaxData))
	}
	if len(r.Commitment) != e2e.FrankingKeyLen {
		return nil, rpcErr(400, "COMMITMENT_INVALID", fmt.Sprintf("commitment of %d bytes, %d expected", len(r.Commitment), e2e.FrankingKeyLen))
	}
	to := c.Other(p.xuid)
	now := s.cfg.Now()
	date, fresh, err := s.chats.AddSent(SentRecord{Chat: c.ID, Sender: p.xuid, Recipient: to, RandomID: r.RandomID, Commitment: r.Commitment,
		Date: int(now.Unix())}, now)
	if err != nil {
		return nil, s.e2eInternal(p, method, err)
	}
	if !fresh {
		log.Debug("mt.e2e_send_repeated " + diag.KV(diag.KVP{K: "chat", V: c.ID}, diag.KVP{K: "from", V: hexID(p.xuid)}) + ": random_id already relayed, the first date answered")
		return &tl.MessagesSentEncryptedMessage{Date: date}, nil
	}
	u := &tl.UpdateNewEncryptedMessage{Message: tl.EncryptedMessage{RandomID: r.RandomID, ChatID: int(c.ID), Date: date, Bytes: r.Data, Commitment: r.Commitment}}
	if err := s.storeChatUpdate(to, c.ID, u, true); err != nil {
		_ = s.chats.DeleteSent(c.ID, p.xuid, r.RandomID)
		return nil, s.e2eInternal(p, method, err)
	}
	log.Debug("mt.e2e_relayed " + diag.KV(diag.KVP{K: "chat", V: c.ID}, diag.KVP{K: "from", V: hexID(p.xuid)}, diag.KVP{K: "to", V: hexID(to)},
		diag.KVP{K: "bytes", V: len(r.Data)}))
	return &tl.MessagesSentEncryptedMessage{Date: date}, nil
}

// e2eAck: messages.ackEncrypted: the account's secret-chat updates up to maxPts (sealed bytes with them) are dropped.
func (s *Server) e2eAck(p party, maxPts int) error {
	if maxPts <= 0 {
		return nil
	}
	n, err := s.store.AckChat(p.xuid, maxPts)
	if err != nil {
		return s.e2eInternal(p, "messages.ackEncrypted", err)
	}
	if n > 0 {
		log.Debug("mt.e2e_acked " + diag.KV(diag.KVP{K: "xuid", V: hexID(p.xuid)}, diag.KVP{K: "max_pts", V: maxPts}, diag.KVP{K: "dropped", V: n}))
	}
	return nil
}

// e2eReport: messages.reportEncrypted (contract 2.7). Only the recipient can report a message; a matching commitment puts it on the
// operators' list and nothing else happens.
func (s *Server) e2eReport(p party, r *tl.MessagesReportEncryptedRequest) error {
	const method = "messages.reportEncrypted"
	rec, err := s.chats.SentTo(int32(r.ChatID), r.RandomID, p.xuid)
	if err != nil {
		return s.e2eInternal(p, method, err)
	}
	if rec == nil {
		return rpcErr(400, "FRANKING_MISMATCH", fmt.Sprintf("no commitment kept for a message to the caller on chat %d (none, not its recipient, or older than %s)", r.ChatID, SentTTL))
	}
	if len(r.Text) > e2e.MaxData || !e2e.CheckCommitment(rec.Commitment, r.FrankingKey, rec.Sender, rec.Recipient, rec.RandomID, r.Text) {
		return rpcErr(400, "FRANKING_MISMATCH", fmt.Sprintf("text (%d bytes) and franking key (%d bytes) do not match the commitment of chat %d", len(r.Text),
			len(r.FrankingKey), r.ChatID))
	}
	now := s.cfg.Now()
	fresh, err := s.chats.AddReport(Report{Created: now, Reporter: p.xuid, Sender: rec.Sender, Chat: rec.Chat, RandomID: rec.RandomID, Text: r.Text,
		Reason: clipRunes(r.Reason, maxReportReason)})
	if err != nil {
		return s.e2eInternal(p, method, err)
	}
	log.Info("mt.e2e_reported " + diag.KV(diag.KVP{K: "chat", V: rec.Chat}, diag.KVP{K: "reporter", V: hexID(p.xuid)}, diag.KVP{K: "sender", V: hexID(rec.Sender)},
		diag.KVP{K: "text_bytes", V: len(r.Text)}, diag.KVP{K: "again", V: !fresh}) + ": the commitment matched; on the operators' report list, no action taken")
	return nil
}

func clipRunes(s string, n int) string {
	if r := []rune(s); len(r) > n {
		return string(r[:n])
	}
	return s
}

// FriendshipEnded discards the secret chats of two players who are no longer friends (the friends API calls it after a removal).
func (s *Server) FriendshipEnded(a, b int64) {
	if s.chats == nil {
		return
	}
	s.e2eMu.Lock()
	defer s.e2eMu.Unlock()
	chats, err := s.chats.OpenChats(a, b)
	if err != nil {
		log.Exc("mt.e2e_friendship_ended "+diag.KV(diag.KVP{K: "a", V: hexID(a)}, diag.KVP{K: "b", V: hexID(b)})+": their chats were not read, none discarded", err)
		return
	}
	for _, c := range chats {
		s.discardLocked(c, "the friendship ended")
	}
}

// Reports lists the newest end-to-end reports (the operators' list).
func (s *Server) Reports(limit int) ([]Report, error) {
	if s.chats == nil {
		return nil, nil
	}
	return s.chats.Reports(limit)
}

// SweepStores drops what outlived its time: stored updates (sealed bytes with them) after UpdateTTL, commitments and discarded chats after
// SentTTL. The janitor runs it every hour.
func (s *Server) SweepStores(now time.Time) {
	n, err := s.store.Expire()
	if err != nil {
		log.Exc("mt.sweep_failed: stored updates older than "+UpdateTTL.String()+" not dropped", err, diag.Warn)
	}
	var sent, chats int64
	if s.chats != nil {
		if sent, chats, err = s.chats.Sweep(now); err != nil {
			log.Exc("mt.sweep_failed: commitments and discarded chats older than "+SentTTL.String()+" not dropped", err, diag.Warn)
		}
	}
	if n > 0 || sent > 0 || chats > 0 {
		log.Info("mt.swept " + diag.KV(diag.KVP{K: "updates", V: n}, diag.KVP{K: "commitments", V: sent}, diag.KVP{K: "discarded_chats", V: chats}) +
			fmt.Sprintf(": updates older than %s (their clients did not come back), commitments and discarded chats older than %s", UpdateTTL, SentTTL))
	}
}

// Local is an account that lives in this process instead of behind a session (the test server's demo friends). It calls the end-to-end
// functions under the same rules as a client; its stored updates wake it (Wake) and its updates without pts arrive on Shorts.
type Local struct {
	s      *Server
	p      party
	Wake   chan struct{}
	Shorts chan tl.UpdateClass

	mu  sync.Mutex
	pts int
}

// NewLocal makes the in-process account xuid with chat device id device; accountKey is the key its device certificates are checked against.
func (s *Server) NewLocal(xuid, device int64, accountKey ed25519.PublicKey) *Local {
	l := &Local{s: s, p: party{xuid: xuid, device: device, accountKey: accountKey}, Wake: make(chan struct{}, 1), Shorts: make(chan tl.UpdateClass, 64)}
	s.mu.Lock()
	s.locals[xuid] = l
	s.mu.Unlock()
	return l
}

func (s *Server) local(x int64) *Local {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.locals[x]
}

func (l *Local) poke() {
	select {
	case l.Wake <- struct{}{}:
	default:
	}
}

func (l *Local) short(u tl.UpdateClass) {
	select {
	case l.Shorts <- u:
	default:
	}
}

func (l *Local) RegisterDevice(pub, sig []byte) (*tl.ChatDevice, error) {
	return l.s.e2eRegisterDevice(l.p, pub, sig)
}
func (l *Local) Devices(user int64) (*tl.UsersDevices, error) { return l.s.e2eDevices(l.p, user) }
func (l *Local) DhConfig(version, n int) (tl.MessagesDhConfigClass, error) {
	return l.s.e2eDhConfig(version, n)
}
func (l *Local) RequestEncryption(r *tl.MessagesRequestEncryptionRequest) (tl.EncryptedChatClass, error) {
	return l.s.e2eRequest(l.p, r)
}
func (l *Local) AcceptEncryption(r *tl.MessagesAcceptEncryptionRequest) (tl.EncryptedChatClass, error) {
	return l.s.e2eAccept(l.p, r)
}
func (l *Local) DiscardEncryption(chat int) error { return l.s.e2eDiscard(l.p, chat) }
func (l *Local) SendEncrypted(r *tl.MessagesSendEncryptedRequest) (*tl.MessagesSentEncryptedMessage, error) {
	return l.s.e2eSend(l.p, r)
}

// Updates returns the stored updates since the last call, in pts order, and acks them (secret-chat rows included: they are in hand).
func (l *Local) Updates() ([]tl.UpdateClass, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	var out []tl.UpdateClass
	for {
		rows, _, err := l.s.store.Since(l.p.xuid, l.pts, maxDifference)
		if err != nil {
			return out, err
		}
		for _, r := range rows {
			u, err := tl.DecodeUpdate(&bin.Buffer{Buf: r.Body})
			if err != nil {
				return out, fmt.Errorf("stored update pts %d does not decode: %w", r.Pts, err)
			}
			out = append(out, u)
			l.pts = r.Pts
		}
		if len(rows) < maxDifference {
			break
		}
	}
	_ = l.s.store.Ack(l.p.xuid, l.pts)
	_, _ = l.s.store.AckChat(l.p.xuid, l.pts)
	return out, nil
}
