package mt

import "fh2mp/internal/auth"

// Purpose is the sign-in purpose bound into the signature (auth.SignMessage), so a launcher sign-in can never be replayed as a chat one.
const Purpose = "mtproto"

// AuthAccounts is Accounts on the LSP's identity service.
type AuthAccounts struct{ Svc *auth.Service }

func (a AuthAccounts) Challenge(ip string) ([]byte, float64, error) { return a.Svc.ChallengeRaw(ip) }

func (a AuthAccounts) SignIn(ip string, chal, pub, sig []byte, fields ...[]byte) (int64, string, error) {
	acct, err := a.Svc.SignInKey(ip, Purpose, chal, pub, sig, fields...)
	if err != nil {
		return 0, "", err
	}
	return acct.Xuid, acct.Gamertag, nil
}
