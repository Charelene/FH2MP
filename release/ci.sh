#!/usr/bin/env bash
# Every check, one entry point for GitHub (.github/workflows/ci.yml, release.yml) and for a developer machine.
#
#   release/ci.sh [--container] <lane>...
#
# Lanes: server (portable server checks, release flow, pure shim checks), pg (disposable PostgreSQL), shim (Wine checks),
# launcher, mutations (deliberate regressions must be caught), quick (server + launcher, before a push), all.
# --container runs the lanes in the image built from release/ci.Containerfile (podman or docker): the same Python, Wine, mingw and
# PostgreSQL as GitHub. Without it the host's tools are used, which can differ (host Wine 10/11 vs Debian's, Python 3.14 vs 3.12).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

container=0
lanes=()
for a in "$@"; do
    case "$a" in
        --container) container=1 ;;
        server|pg|shim|launcher|mutations|quick|all) lanes+=("$a") ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "unknown lane or option: $a" >&2; exit 2 ;;
    esac
done
[ ${#lanes[@]} -gt 0 ] || { sed -n '2,9p' "$0" >&2; exit 2; }

if [ "$container" = 1 ]; then
    engine=$(command -v podman || command -v docker || true)
    [ -n "$engine" ] || { echo "--container needs podman or docker" >&2; exit 2; }
    # the tag names the image's inputs, so an edited Containerfile or requirement file builds a new one
    tag=fh2mp-ci:$(cat release/ci.Containerfile release/requirements-sign.txt release/requirements-test.txt launcher/requirements-release.txt \
                   | sha256sum | cut -c1-16)
    "$engine" image exists "$tag" 2>/dev/null || "$engine" image inspect "$tag" >/dev/null 2>&1 \
        || "$engine" build -f release/ci.Containerfile -t "$tag" .
    opts=(--rm -v "$root:/src:Z" -w /src)
    [ "$(basename "$engine")" = podman ] && opts+=(--userns=keep-id)
    exec "$engine" run "${opts[@]}" "$tag" release/ci.sh "${lanes[@]}"
fi

export PYTHONDONTWRITEBYTECODE=1
py=${PYTHON:-python3}
started=$(date +%s)
summary=()
run() {   # run <name> <command...>: time it and remember the result
    local name=$1 t0 rc
    shift
    t0=$(date +%s)
    [ -z "${GITHUB_ACTIONS:-}" ] || echo "::group::$name"
    set +e
    "$@"
    rc=$?
    set -e
    [ -z "${GITHUB_ACTIONS:-}" ] || echo "::endgroup::"
    summary+=("$(printf '%-28s %s %4ss' "$name" "$([ $rc = 0 ] && echo pass || echo FAIL)" $(( $(date +%s) - t0 )))")
    [ $rc = 0 ] || failed=1
}
failed=0

lane_server() {
    run "server checks" "$py" -W ignore -I release/run_server_tests.py
    run "release flow" "$py" -I -m unittest discover -s ops -p test_release_flow.py
    run "shim sha256" shim/test_sha256.sh
    run "shim inflate" shim/test_inflate.sh
    run "shim gamedb keep" shim/test_gamedb_keep.sh
    run "shim session age" shim/test_session_age.sh
}

pg_root=
pg_stop() { [ -z "$pg_root" ] || { pg_ctl -D "$pg_root/data" -m immediate stop >/dev/null 2>&1 || true; rm -rf "$pg_root"; pg_root=; }; }
trap pg_stop EXIT
pg_start() {   # a disposable cluster on a private socket directory; FH2MP_TEST_PG_DSN points at its fh2mp_test_ci database
    local bin
    bin=$(ls /usr/lib/postgresql/*/bin/initdb 2>/dev/null | sort -V | tail -1)
    command -v initdb >/dev/null || { [ -n "$bin" ] && PATH=$(dirname "$bin"):$PATH; } || return 1
    pg_root=$(mktemp -d)
    initdb -D "$pg_root/data" -A trust --no-locale --encoding=UTF8 >/dev/null
    pg_ctl -D "$pg_root/data" -l "$pg_root/postgres.log" -o "-h '' -k $pg_root" -w start >/dev/null
    createdb -h "$pg_root" fh2mp_test_ci
    export FH2MP_TEST_PG_DSN="host=$pg_root dbname=fh2mp_test_ci"
}

lane_pg() {
    if ! pg_start; then
        echo "PostgreSQL is not installed here; use --container" >&2
        summary+=("$(printf '%-28s %s' "postgres checks" "NOT RUN (no PostgreSQL)")")
        failed=1
        return
    fi
    run "postgres checks" "$py" -W ignore -I release/run_server_tests.py --lane postgres
    run "mutations postgres" "$py" -I release/check_test_mutations.py --area postgres
    pg_stop
    unset FH2MP_TEST_PG_DSN
}

lane_shim() {
    # One booted prefix, copied for each check (shim/wine_prefix.sh); a Wine that cannot start is reported as that, not as a failed check.
    local tpl
    tpl=$(mktemp -d)/prefix
    if ! WINEPREFIX="$tpl" WINEDEBUG=-all wineboot -i >/dev/null 2>&1 || ! WINEPREFIX="$tpl" WINEDEBUG=-all wine cmd /c exit >/dev/null 2>&1; then
        echo "Wine cannot start here ($(wine --version 2>&1)); use --container" >&2
        failed=1
        return
    fi
    WINEPREFIX="$tpl" wineserver -w
    export FH2MP_WINE_TEMPLATE="$tpl"
    # The checks share nothing (own prefix, own wineserver), so they run side by side; each one's output is shown only when it fails.
    local t logs t0 rc
    logs=$(dirname "$tpl")
    declare -A pids
    t0=$(date +%s)
    for t in gamedb_files game_image title_scope xhr_config torque_scale net_strings torque_load speed_pb restricted_tools lighting; do
        "shim/test_$t.sh" > "$logs/$t.log" 2>&1 &
        pids[$t]=$!
    done
    for t in "${!pids[@]}"; do
        rc=0
        wait "${pids[$t]}" || rc=$?
        if [ $rc != 0 ]; then
            echo "--- shim $t failed ($rc):" >&2
            cat "$logs/$t.log" >&2
            failed=1
        fi
        summary+=("$(printf '%-28s %s %4ss' "shim $t" "$([ $rc = 0 ] && echo pass || echo FAIL)" $(( $(date +%s) - t0 )))")
    done
    unset FH2MP_WINE_TEMPLATE
    rm -rf "$logs"
}

lane_launcher() {
    run "launcher checks" env FH2MP_NO_KEYRING=1 QT_QPA_PLATFORM=offscreen sh -c 'cd launcher && "$0" -m unittest discover -s tests' "$py"
}

lane_mutations() {
    run "mutations server" "$py" -I release/check_test_mutations.py --area server
    run "mutations launcher" sh -c 'cd launcher && "$0" -I ../release/check_test_mutations.py --area launcher' "$py"
}

for lane in "${lanes[@]}"; do
    case "$lane" in
        quick) lane_server; lane_launcher ;;
        all) lane_server; lane_pg; lane_shim; lane_launcher; lane_mutations ;;
        *) "lane_$lane" ;;
    esac
done

echo
printf '%s\n' "${summary[@]}"
echo "total $(( $(date +%s) - started ))s"
exit $failed
