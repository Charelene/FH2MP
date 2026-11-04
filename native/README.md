# native/ - Go port of the FH2MP game server and LSP

Go module `fh2mp`. The Python code in `server/` stays as the reference and the rollback.

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
- Each Go file names the Python function it replaces in its header comment.

## Rules
- Every world is owned by exactly one goroutine; others send it messages over a channel.
- Same ports, argv, `FH2MP_*` variables, control protocol, log line shape and database schema as the Python code.
- Plan of record: `docs/go-rewrite-plan.md` (private docs).
