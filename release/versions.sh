#!/usr/bin/env bash
# The component versions of a release, from shim/VERSION and release/launcher.json, checked against the tag.
#
#   release/versions.sh --tag v1.5.1 [--full] [--github-output FILE]
#   release/versions.sh                      (no tag: a test build of the shim)
#
# The shim is built at the tag's version when shim/VERSION equals the tag; otherwise it keeps the version of the release it was built in
# and is carried: fh2mp-release manifest-build --carry-dir copies its signed manifest entry, and the release workflow its zip, from that
# release (a launcher-only release; setting shim/VERSION back to an earlier release's version withdraws a shim hotfix the same way).
# The launcher is never built here since 1.5.0: it is Encore, pinned by version and file in release/launcher.json and taken from Encore's
# release (build_launcher is always false); its version is Encore's own, on no FH2MP line.
#
# Rules: the tag is vX.Y.Z or vX.Y.Z-rcN; a carried shim is of the tag's X.Y line, not above it, and a stable X.Y.Z as the tag is; --full
# (release/publish.sh for ops/ship.sh) requires the shim to equal the tag; the launcher pin's version is X.Y.Z or X.Y.Z-rcN.
# kind: release for an X.Y.0 tag (or its release candidate) that builds the shim, hotfix for a later patch or a carried shim.
# Prints key=value lines (shim, launcher, build_shim, build_launcher, kind); --github-output appends them there.
# Exit 1 with the reason when a rule does not hold, 2 for a wrong command line. (Was release/versions.py until 2026-11-14; the launcher's
# version came from launcher/fh2mp_launcher/version.py until 1.5.0.)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tag= full=0 out=
while [ $# -gt 0 ]; do
  case $1 in
    --tag) [ $# -ge 2 ] || { echo "--tag needs a value" >&2; exit 2; }; tag=$2; shift 2;;
    --full) full=1; shift;;
    --github-output) [ $# -ge 2 ] || { echo "--github-output needs a file" >&2; exit 2; }; out=$2; shift 2;;
    --root) [ $# -ge 2 ] || { echo "--root needs a folder" >&2; exit 2; }; root=$2; shift 2;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done
fail() { echo "$1" >&2; exit 1; }
re='^([0-9]+)\.([0-9]+)\.([0-9]+)(-rc([0-9]+))?$'

trim() { local s=$1; s=${s#"${s%%[![:space:]]*}"}; s=${s%"${s##*[![:space:]]}"}; printf '%s' "$s"; }
shim=$(trim "$(cat "$root/shim/VERSION")")
# the pin's top-level "version" (the file's first "version" key; fh2mp-release launcher-pin reads the whole file)
[ -f "$root/release/launcher.json" ] || fail "release/launcher.json is missing: it pins the launcher the manifest names"
launcher=$(sed -n 's/^[[:space:]]*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$root/release/launcher.json" | head -n 1)

# vkey v -> "major minor patch stable rc" (stable 1, a release candidate 0), compared field by field
vkey() {
  [[ $1 =~ $re ]]
  echo "$((10#${BASH_REMATCH[1]})) $((10#${BASH_REMATCH[2]})) $((10#${BASH_REMATCH[3]})) $([ -n "${BASH_REMATCH[4]}" ] && echo 0 || echo 1) $((10#${BASH_REMATCH[5]:-0}))"
}
newer() { # newer a b: a's key above b's
  local -a x y; local i
  read -r -a x <<<"$(vkey "$1")"; read -r -a y <<<"$(vkey "$2")"
  for i in 0 1 2 3 4; do
    [ "${x[i]}" -gt "${y[i]}" ] && return 0
    [ "${x[i]}" -lt "${y[i]}" ] && return 1
  done
  return 1
}
line() { local -a x; read -r -a x <<<"$(vkey "$1")"; echo "${x[0]}.${x[1]}"; }

[[ $shim =~ $re ]] || fail "shim/VERSION is '$shim', not X.Y.Z or X.Y.Z-rcN"
[[ $launcher =~ $re ]] || fail "the launcher pin's version (release/launcher.json) is '$launcher', not X.Y.Z or X.Y.Z-rcN"

build_launcher=false
if [ -z "$tag" ]; then
  build_shim=true kind=release
else
  [[ $tag == v* && ${tag#v} =~ $re ]] || fail "tag '$tag' is not vX.Y.Z or vX.Y.Z-rcN"
  ver=${tag#v}
  build_shim=false
  [ "$shim" = "$ver" ] && build_shim=true
  if [ $full = 1 ] && [ $build_shim = false ]; then
    fail "version mismatch: tag $ver, shim $shim (a full release sets shim/VERSION to the tag)"
  fi
  if [ $build_shim = false ]; then
    [ "$(line "$shim")" = "$(line "$ver")" ] || fail "the shim ($shim) is not of the $ver line: only a full release changes X.Y"
    newer "$shim" "$ver" && fail "the shim ($shim) is newer than the tag $ver"
    [[ $shim == *-* || $ver == *-* ]] && fail "a carried shim needs stable versions (tag $ver, shim $shim): release candidates build it"
  fi
  [[ $ver =~ $re ]]
  if [ $build_shim = true ] && [ "$((10#${BASH_REMATCH[3]}))" = 0 ]; then kind=release; else kind=hotfix; fi
fi
lines=$(printf 'shim=%s\nlauncher=%s\nbuild_shim=%s\nbuild_launcher=%s\nkind=%s\n' "$shim" "$launcher" "$build_shim" "$build_launcher" "$kind")
printf '%s\n' "$lines"
[ -z "$out" ] || printf '%s\n' "$lines" >> "$out"
