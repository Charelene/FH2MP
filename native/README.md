# native/ - the FH2MP game server and LSP

Go module `fh2mp`. It replaced the Python servers (`server/`), which were removed on 2026-11-07, after the 1.2.0 release; they are in the git
history up to that point. A package comment like `(= server/x.py)` names the Python module the package was ported from.

## Build, test, run
    cd native
    go build ./...
    go vet ./...
    gofmt -l .
    go test -race ./...
    GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -o bin/fh2mp-server ./cmd/fh2mp-server

## Reading guide
- Game server: `cmd/fh2mp-server/main.go` (flags, env), then `internal/game/hub.go` (where a datagram goes), then `internal/game/world.go` (one world).
- LSP: `cmd/fh2mp-lsp/main.go`, then `internal/lsp/router.go` (which handler gets which request).

## Rules
- Every world is owned by exactly one goroutine; the hub goroutine changes shared tables only while the workers are paused (`internal/game/workers.go`).
- Same ports, argv, `FH2MP_*` variables, control protocol, log line shape and database schema as the Python servers had: the players' shims and
  launchers in the field depend on them.
- The recorded fixtures in `testdata/` are frozen: their recorder needed the Python servers. A change that alters one edits the expected output
  by hand and says why in the commit.

## Kill switches (live ops)

A realm's features file (`FH2MP_FEATURES_FILE`, the same path in the LSP's and the game server's environment; documented in `internal/flags`) switches
features off without a release. Both servers look at it again each second, so a write takes effect in about a second and nothing restarts.

```json
{"voice": true, "nos": true, "mt_chat": true, "lobbies": true}
```

| Switch | Off means |
| --- | --- |
| `voice` | the game server reads chat packets and drops them (nobody hears anybody) |
| `nos` | NitroSystem's NOS may burn nowhere (like `FH2MP_NO_NOS=1`) |
| `mt_chat` | the chat server closes new connections at once and the open ones within 30 s |
| `lobbies` | no ticket is matched and no seat reserved (503 `lobbies_off`); players already in a world stay |

A feature missing from the file, a missing file and an unset path all mean on, which is what the servers did before. A broken file keeps the last good
switches; an unknown key or a value that is not `true`/`false` is logged and ignored while the rest applies. `PUT /ops/v1/config {"features": {"voice": false}}`
writes the file; `GET /ops/v1/config` and `GET /v1/status` show the switches in force. The operator's maintenance text stays in `launcher.json`
(`maintenance`, also settable with `PUT /ops/v1/config`).
