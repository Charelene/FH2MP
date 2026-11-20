#!/usr/bin/env bash
# release/versions.sh against version files in a temporary tree: what each tag builds, and every refusal with its reason.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/shim" "$tmp/release"
failed=0
pin() { # pin <launcher version>: release/launcher.json as it is written (version first, the assets after it)
  printf '{\n "version": "%s",\n "minLauncher": "2.1.0",\n "assets": {\n  "windows-x64-installer": {"name": "EncoreSetup.exe", "sha256": "", "size": 0}\n }\n}\n' "$1" > "$tmp/release/launcher.json"
}
check() { # check <description> <shim> <launcher> <want exit> <want text> -- args...
  local what=$1 shim=$2 app=$3 want_rc=$4 want=$5 rc out
  shift 6
  printf '%s\n' "$shim" > "$tmp/shim/VERSION"
  pin "$app"
  set +e
  out=$(release/versions.sh --root "$tmp" "$@" 2>&1)
  rc=$?
  set -e
  if [ "$rc" != "$want_rc" ] || [[ $out != *"$want"* ]]; then
    printf 'FAIL %s: exit %s (want %s)\n  got: %s\n  want: %s\n' "$what" "$rc" "$want_rc" "$out" "$want"
    failed=1
  fi
}
nl=$'\n'
check "no tag: a test build of the shim" 1.5.0 2.1.0 0 "shim=1.5.0${nl}launcher=2.1.0${nl}build_shim=true${nl}build_launcher=false${nl}kind=release" --
check "full release" 1.5.0 2.1.0 0 "shim=1.5.0${nl}launcher=2.1.0${nl}build_shim=true${nl}build_launcher=false${nl}kind=release" -- --tag v1.5.0 --full
check "release candidate" 1.6.0-rc1 2.2.0-rc1 0 "build_shim=true${nl}build_launcher=false${nl}kind=release" -- --tag v1.6.0-rc1
check "shim hotfix" 1.5.1 2.1.0 0 "build_shim=true${nl}build_launcher=false${nl}kind=hotfix" -- --tag v1.5.1
check "launcher-only release: the shim is carried" 1.5.0 2.1.1 0 "shim=1.5.0${nl}launcher=2.1.1${nl}build_shim=false${nl}build_launcher=false${nl}kind=hotfix" -- --tag v1.5.1
check "the launcher is on no FH2MP line" 1.5.1 3.0.0 0 "launcher=3.0.0" -- --tag v1.5.1
check "full needs the shim" 1.5.0 2.1.0 1 "version mismatch: tag 1.5.1, shim 1.5.0 (a full release sets shim/VERSION to the tag)" -- --tag v1.5.1 --full
check "another X.Y line" 1.4.8 2.1.0 1 "the shim (1.4.8) is not of the 1.5.0 line: only a full release changes X.Y" -- --tag v1.5.0
check "carried newer than the tag" 1.5.3 2.1.0 1 "the shim (1.5.3) is newer than the tag 1.5.2" -- --tag v1.5.2
check "carried release candidate" 1.5.0-rc2 2.1.0 1 "a carried shim needs stable versions (tag 1.5.1, shim 1.5.0-rc2): release candidates build it" -- --tag v1.5.1
check "release candidate tag carrying" 1.5.0 2.1.0 1 "a carried shim needs stable versions (tag 1.5.1-rc1, shim 1.5.0)" -- --tag v1.5.1-rc1
check "an rc is below its release" 1.5.1-rc3 2.1.0 1 "a carried shim needs stable versions" -- --tag v1.5.1
check "bad tag" 1.5.1 2.1.0 1 "tag '1.5.1' is not vX.Y.Z or vX.Y.Z-rcN" -- --tag 1.5.1
check "bad shim version" 1.5 2.1.0 1 "shim/VERSION is '1.5', not X.Y.Z or X.Y.Z-rcN" -- --tag v1.5.1
check "bad launcher version" 1.5.1 dev 1 "the launcher pin's version (release/launcher.json) is 'dev', not X.Y.Z or X.Y.Z-rcN" -- --tag v1.5.1
check "leading zeros are numbers" 1.5.09 2.1.0 0 "kind=hotfix" -- --tag v1.5.10
check "unknown option" 1.5.1 2.1.0 2 "unknown option: --nope" -- --nope
rm -f "$tmp/release/launcher.json"
set +e; out=$(release/versions.sh --root "$tmp" --tag v1.5.1 2>&1); rc=$?; set -e
[ $rc = 1 ] && [[ $out == *"release/launcher.json is missing"* ]] || { echo "FAIL no pin: $rc $out"; failed=1; }
# --github-output appends the same lines
printf '1.5.1\n' > "$tmp/shim/VERSION"; pin 2.1.0
printf 'before=1\n' > "$tmp/out"
release/versions.sh --root "$tmp" --tag v1.5.1 --github-output "$tmp/out" >/dev/null
[ "$(cat "$tmp/out")" = "before=1${nl}shim=1.5.1${nl}launcher=2.1.0${nl}build_shim=true${nl}build_launcher=false${nl}kind=hotfix" ] \
  || { echo "FAIL --github-output: $(cat "$tmp/out")"; failed=1; }
# the real tree reads too (shim/VERSION and release/launcher.json as they are written)
release/versions.sh >/dev/null || { echo "FAIL: the checkout's own version files"; failed=1; }
[ $failed = 0 ] && echo "release versions: all checks pass"
exit $failed
