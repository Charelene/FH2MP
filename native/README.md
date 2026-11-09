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
