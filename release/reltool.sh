# Sourced by the release scripts (run from the checkout's root):
#   reltool <command> ...           native/cmd/fh2mp-release on this machine, built into build/release-tool (Go's build cache keeps it fast)
#   reltool_for_host <arch> <out>   the same command for the server (linux, arch = uname -m there: aarch64 or x86_64)
# FH2MP_RELEASE_TOOL names a binary to use instead (tests).
reltool() {
  local bin=${FH2MP_RELEASE_TOOL:-}
  if [ -z "$bin" ]; then
    bin=$PWD/build/release-tool/fh2mp-release
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) bin=$bin.exe;; esac
    command -v go >/dev/null || { echo "the release checks need the Go toolchain on this machine" >&2; return 1; }
    (cd native && go build -o "$bin" ./cmd/fh2mp-release) || { echo "could not build native/cmd/fh2mp-release" >&2; return 1; }
  fi
  "$bin" "$@"
}
reltool_for_host() {
  local arch
  case $1 in aarch64|arm64) arch=arm64;; x86_64|amd64) arch=amd64;; *) echo "no Go build for a $1 server" >&2; return 1;; esac
  command -v go >/dev/null || { echo "the release checks need the Go toolchain on this machine" >&2; return 1; }
  (cd native && CGO_ENABLED=0 GOOS=linux GOARCH=$arch go build -o "$2" ./cmd/fh2mp-release)
}
