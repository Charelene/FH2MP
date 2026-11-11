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
