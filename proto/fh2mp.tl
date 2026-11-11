// FH2MP chat API, carried over MTProto 2.0. Our own schema: no Telegram API type is used or accepted.
// Layer 1. A constructor id is the CRC32 (IEEE) of its definition without the id, Vector<X> written as "Vector X"
// (Telegram's rule); native/internal/mt/tl/schema_test.go checks every id and that none collides with an MTProto service type.
// Generated code: native/internal/mt/tl (gotdgen), client/crates/fh2mp-tl (grammers-tl-gen).

boolFalse#bc799737 = Bool;
boolTrue#997275b5 = Bool;
true#3fedd339 = True;
vector#1cb5c415 {t:Type} # [ t ] = Vector t;

user#87075b8d id:long gamertag:string = User;

// Limits and clock of this server, answered to help.init.
help.config#d80277fe layer:int date:int max_request_bytes:int key_ttl:int = help.Config;

// A sign-in challenge: the client signs it with the account key (auth.signIn). Single use, valid expires_in seconds.
auth.challenge#bef80cdf challenge:bytes realm:string expires_in:int = auth.Challenge;
auth.authorization#ff036af1 user:User = auth.Authorization;

// The account's update sequence: every stored update raises pts by one.
updates.state#2294f8c3 pts:int date:int = updates.State;
updates.differenceEmpty#d2c29bf4 pts:int date:int = updates.Difference;
updates.difference#b73ab494 new_updates:Vector<Update> state:updates.State = updates.Difference;
updates.differenceSlice#78c2ca90 new_updates:Vector<Update> intermediate_state:updates.State = updates.Difference;
updates.differenceTooLong#4afe8f6d pts:int = updates.Difference;

// A notice from the server operators (stored, carries pts).
updateNotice#bb869d86 id:long text:string date:int = Update;
// This session was signed out (pushed without pts, then the server ends the session): another sign-in, a ban or a revoke.
updateLoggedOut#c33bab82 reason:string = Update;

// Pushed to a signed-in session. updates: stored updates pts-pts_count+1 .. pts; updateShort: one update without pts;
// updatesTooLong: the client calls updates.getDifference.
updates#6b9f7239 updates:Vector<Update> pts:int pts_count:int date:int = Updates;
updateShort#78d4dec1 update:Update date:int = Updates;
updatesTooLong#e317af7e = Updates;

// Friends, presence and club chat (the chat core's friend list and club tab). Club chat stays readable by the server (moderation); direct
// messages are end-to-end and come in their own part of the schema.
presenceOffline#5e53c43f = Presence;
// The chat core or the launcher is open.
presenceOnline#1622a074 = Presence;
// In the game; lobby_kind is freeroam or roadtrip when the player is in a lobby, else empty.
presencePlaying#b935baca lobby_kind:string = Presence;

club#bab49076 id:long tag:string name:string role:string members:int = Club;
contact#8fdfd8ba flags:# user:User level:int gamerpic_version:long presence:Presence club:flags.0?Club = Contact;
friends.list#65149329 me:Contact friends:Vector<Contact> incoming:Vector<Contact> outgoing:Vector<Contact> = friends.List;

clubMessage#3a419090 id:long club_id:long sender:User text:string date:int = ClubMessage;
// read_max_id: the newest message this account has marked read.
clubs.history#eb4fafe0 messages:Vector<ClubMessage> read_max_id:long = clubs.History;

users.gamerpic#c7416ef9 user_id:long version:long png:bytes = users.Gamerpic;

// Pushed without pts: presence of a friend, a new club message (fetch clubs.getHistory after the last id seen to catch up after a
// reconnect), a change of the friend list or requests (fetch friends.get).
updateUserPresence#806504bc user_id:long presence:Presence = Update;
updateClubMessage#5a6f6195 message:ClubMessage = Update;
updateFriends#1bfadb02 = Update;

---functions---

// The first call of every session: names the client's layer. Other API calls before it get 400 INIT_REQUIRED.
help.init#9e6180e9 layer:int app_version:string platform:string = help.Config;

auth.getChallenge#c311e549 = auth.Challenge;
// signature = Ed25519 over "FH2MP-AUTH-v1\0" realm "\0mtproto\0" challenge {u16 BE length, field} for pubkey, auth_key_id (8 bytes as on the
// wire), device_id (8 bytes little-endian): the session is bound to this auth key and to the account that owns pubkey.
auth.signIn#e245fa6e challenge:bytes pubkey:bytes signature:bytes device_id:long = auth.Authorization;
auth.logOut#5717da40 = Bool;

updates.getState#edd4882a = updates.State;
updates.getDifference#6a5bfec6 pts:int limit:int = updates.Difference;

friends.get#5e6406b0 = friends.List;
// query: a gamertag, or an id (decimal, or hex with 0x).
friends.add#46162cb9 query:string = Bool;
friends.accept#74761aa3 user_id:long = Bool;
// Declines an incoming request or cancels an outgoing one.
friends.decline#ae994302 user_id:long = Bool;
friends.remove#6e78f556 user_id:long = Bool;

// Messages after after_id, oldest first (after_id 0: the newest page). The club must be the caller's.
clubs.getHistory#4ebf1730 club_id:long after_id:long limit:int = clubs.History;
// random_id makes a resend after a lost answer post once: the same random_id from the same account answers the first message.
clubs.sendMessage#17becd97 club_id:long random_id:long text:string = ClubMessage;
clubs.readHistory#77932beb club_id:long max_id:long = Bool;

users.getGamerpic#9fb5567b user_id:long = users.Gamerpic;
