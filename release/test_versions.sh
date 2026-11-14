#!/usr/bin/env bash
# release/versions.sh against version files in a temporary tree: what each tag builds, and every refusal with its reason.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/shim" "$tmp/launcher/fh2mp_launcher"
failed=0
check() { # check <description> <shim> <launcher> <want exit> <want text> -- args...
  local what=$1 shim=$2 app=$3 want_rc=$4 want=$5 rc out
  shift 6
  printf '%s\n' "$shim" > "$tmp/shim/VERSION"
  printf '__version__ = "%s"\n' "$app" > "$tmp/launcher/fh2mp_launcher/version.py"
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
check "no tag: a test build of both" 1.4.1 1.4.1 0 "shim=1.4.1${nl}launcher=1.4.1${nl}build_shim=true${nl}build_launcher=true${nl}kind=release" --
check "full release" 1.4.0 1.4.0 0 "build_shim=true${nl}build_launcher=true${nl}kind=release" -- --tag v1.4.0 --full
check "release candidate" 1.5.0-rc1 1.5.0-rc1 0 "kind=release" -- --tag v1.5.0-rc1
check "shim hotfix" 1.4.1 1.4.0 0 "build_shim=true${nl}build_launcher=false${nl}kind=hotfix" -- --tag v1.4.1
check "launcher hotfix" 1.4.0 1.4.1 0 "build_shim=false${nl}build_launcher=true${nl}kind=hotfix" -- --tag v1.4.1
check "both rebuilt in a patch" 1.4.1 1.4.1 0 "kind=release" -- --tag v1.4.1
check "rollback: neither equals the tag" 1.4.0 1.4.0 0 "build_shim=false${nl}build_launcher=false${nl}kind=rollback" -- --tag v1.4.2
check "full needs both" 1.4.1 1.4.0 1 "version mismatch: tag 1.4.1, shim 1.4.1, launcher 1.4.0 (a full release sets both to the tag)" -- --tag v1.4.1 --full
check "another X.Y line" 1.2.1 1.1.0 1 "the shim (1.2.1) is not of the 1.1.1 line: only a full release changes X.Y" -- --tag v1.1.1
check "carried newer than the tag" 1.4.3 1.4.2 1 "the shim (1.4.3) is newer than the tag 1.4.2" -- --tag v1.4.2
check "carried release candidate" 1.4.0-rc2 1.4.1 1 "a carried shim needs stable versions (tag 1.4.1, shim 1.4.0-rc2): release candidates build both" -- --tag v1.4.1
check "release candidate tag carrying" 1.4.0 1.4.1-rc1 1 "a carried shim needs stable versions (tag 1.4.1-rc1, shim 1.4.0)" -- --tag v1.4.1-rc1
check "an rc is below its release" 1.4.1-rc3 1.4.1 1 "a carried shim needs stable versions" -- --tag v1.4.1
check "bad tag" 1.4.1 1.4.1 1 "tag '1.4.1' is not vX.Y.Z or vX.Y.Z-rcN" -- --tag 1.4.1
check "bad shim version" 1.4 1.4.1 1 "shim/VERSION is '1.4', not X.Y.Z or X.Y.Z-rcN" -- --tag v1.4.1
check "bad launcher version" 1.4.1 dev 1 "launcher version.py is 'dev', not X.Y.Z or X.Y.Z-rcN" -- --tag v1.4.1
check "leading zeros are numbers" 1.4.09 1.4.10 0 "kind=hotfix" -- --tag v1.4.10
check "unknown option" 1.4.1 1.4.1 2 "unknown option: --nope" -- --nope
# --github-output appends the same lines
printf '1.4.1\n' > "$tmp/shim/VERSION"; printf '__version__ = "1.4.0"\n' > "$tmp/launcher/fh2mp_launcher/version.py"
printf 'before=1\n' > "$tmp/out"
release/versions.sh --root "$tmp" --tag v1.4.1 --github-output "$tmp/out" >/dev/null
[ "$(cat "$tmp/out")" = "before=1${nl}shim=1.4.1${nl}launcher=1.4.0${nl}build_shim=true${nl}build_launcher=false${nl}kind=hotfix" ] \
  || { echo "FAIL --github-output: $(cat "$tmp/out")"; failed=1; }
# the real tree reads too (the launcher's version.py as it is written)
release/versions.sh >/dev/null || { echo "FAIL: the checkout's own version files"; failed=1; }
[ $failed = 0 ] && echo "release versions: all checks pass"
exit $failed
