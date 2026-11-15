#!/usr/bin/env bash
# The component versions of a release, from shim/VERSION and launcher/fh2mp_launcher/version.py, checked against the tag.
#
#   release/versions.sh --tag v1.1.6 [--full] [--github-output FILE]
#   release/versions.sh                      (no tag: a test build of both)
#
# A full release builds the shim and the launcher at the tag's version. A hotfix (a patch tag of the same X.Y line, cut from a hotfix/X.Y branch
# that starts at the release tag) builds only the component whose version file was set to the tag. The other keeps the version of the
# release it was built in and is carried: release/make_manifest.py --carry-dir copies its signed manifest entry, and the release workflow
# copies its files, from that release. Setting shim/VERSION back to an earlier release's version withdraws a shim hotfix the same way.
#
# Rules: the tag is vX.Y.Z or vX.Y.Z-rcN; both versions are of its X.Y line and not above it; a carried version is a stable X.Y.Z and the tag
# is too; --full (release/publish.sh for ops/ship.sh) requires both to equal the tag.
# Prints key=value lines (shim, launcher, build_shim, build_launcher, kind = release | hotfix | rollback); --github-output appends them there.
# Exit 1 with the reason when a rule does not hold, 2 for a wrong command line. (Was release/versions.py until 2026-11-14; same rules and text.)
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
launcher=$(sed -n "s/^__version__[[:space:]]*=[[:space:]]*[\"']\([^\"']*\)[\"'].*/\1/p" "$root/launcher/fh2mp_launcher/version.py" | head -n 1)

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
[[ $launcher =~ $re ]] || fail "launcher version.py is '$launcher', not X.Y.Z or X.Y.Z-rcN"

if [ -z "$tag" ]; then
  build_shim=true build_launcher=true kind=release
else
  [[ $tag == v* && ${tag#v} =~ $re ]] || fail "tag '$tag' is not vX.Y.Z or vX.Y.Z-rcN"
  ver=${tag#v}
  build_shim=false build_launcher=false
  [ "$shim" = "$ver" ] && build_shim=true
  [ "$launcher" = "$ver" ] && build_launcher=true
  if [ $full = 1 ] && { [ $build_shim = false ] || [ $build_launcher = false ]; }; then
    fail "version mismatch: tag $ver, shim $shim, launcher $launcher (a full release sets both to the tag)"
  fi
  for name in shim launcher; do
    v=${!name}; b=build_$name
    [ "${!b}" = true ] && continue
    [ "$(line "$v")" = "$(line "$ver")" ] || fail "the $name ($v) is not of the $ver line: only a full release changes X.Y"
    newer "$v" "$ver" && fail "the $name ($v) is newer than the tag $ver"
    [[ $v == *-* || $ver == *-* ]] && fail "a carried $name needs stable versions (tag $ver, $name $v): release candidates build both"
  done
  if [ $build_shim = true ] && [ $build_launcher = true ]; then kind=release
  elif [ $build_shim = true ] || [ $build_launcher = true ]; then kind=hotfix
  else kind=rollback; fi
fi
lines=$(printf 'shim=%s\nlauncher=%s\nbuild_shim=%s\nbuild_launcher=%s\nkind=%s\n' "$shim" "$launcher" "$build_shim" "$build_launcher" "$kind")
printf '%s\n' "$lines"
[ -z "$out" ] || printf '%s\n' "$lines" >> "$out"
