#!/usr/bin/env bash
# Every check, one entry point for GitHub (.github/workflows/ci.yml, release.yml) and for a developer machine.
#
#   release/ci.sh [--container] <lane>...
#
# Lanes: go (the game server and LSP in native/: fmt, vet, tests on a disposable PostgreSQL), unit (release flow, pure shim checks),
# shim (Wine checks), launcher, mutations (deliberate launcher regressions must be caught), quick (go + unit + launcher, before a push), all.
# The Python servers in server/ were removed on 2026-11-07; the Go tests replay their frozen fixtures (native/testdata).
# --container runs the lanes in the image built from release/ci.Containerfile (podman or docker): the same Go, Python, Wine, mingw and
# PostgreSQL as GitHub. Without it the host's tools are used, which can differ (host Wine 10/11 vs Debian's, Python 3.14 vs 3.12).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

container=0
lanes=()
for a in "$@"; do
    case "$a" in
        --container) container=1 ;;
        unit|shim|launcher|mutations|go|quick|all) lanes+=("$a") ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "unknown lane or option: $a" >&2; exit 2 ;;
    esac
done
[ ${#lanes[@]} -gt 0 ] || { sed -n '2,10p' "$0" >&2; exit 2; }

if [ "$container" = 1 ]; then
    # The GitHub image action builds/loads Docker's image; use the same engine
    # there even when the runner also happens to have Podman installed.
    if [ "${GITHUB_ACTIONS:-}" = true ]; then
        engine=$(command -v docker || true)
    else
        engine=$(command -v podman || command -v docker || true)
    fi
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

lane_unit() {
    [ ! -d ops ] || run "release flow" "$py" -I -m unittest discover -s ops -p test_release_flow.py   # ops/ is private: present on the operator machine only
    run "shim sha256" shim/tests/test_sha256.sh
    run "shim inflate" shim/tests/test_inflate.sh
    run "shim gamedb keep" shim/tests/test_gamedb_keep.sh
    run "shim session age" shim/tests/test_session_age.sh
    run "shim session switches" shim/tests/test_session_switches.sh
    run "shim health" shim/tests/test_health.sh
    run "shim eo timelock" shim/tests/test_eo_timelock.sh
    run "shim eo routes" shim/tests/test_eo_routes.sh
    run "shim identity scope" shim/tests/test_identity_scope.sh
    run "shim tune gate" shim/tests/test_tune_gate.sh
    run "shim model unload guard" shim/tests/test_model_unload.sh
    run "shim dtconv guard" shim/tests/test_dtconv.sh
    run "shim online setup" shim/tests/test_online_setup.sh
}

pg_root=
pg_stop() { [ -z "$pg_root" ] || { pg_ctl -D "$pg_root/data" -m immediate stop >/dev/null 2>&1 || true; rm -rf "$pg_root"; pg_root=; }; }
trap pg_stop EXIT
pg_start() {   # a disposable cluster on a loopback port; FH2MP_TEST_PG_ADMIN_DSN names it (the Go tests make their own databases through it,
               # native/internal/dbtest)
    local bin port=
    bin=$(ls /usr/lib/postgresql/*/bin/initdb 2>/dev/null | sort -V | tail -1)
    command -v initdb >/dev/null || { [ -n "$bin" ] && PATH=$(dirname "$bin"):$PATH; } || return 1
    pg_root=$(mktemp -d)
    initdb -D "$pg_root/data" -A trust --no-locale --encoding=UTF8 >/dev/null
    port=$("$py" -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    pg_ctl -D "$pg_root/data" -l "$pg_root/postgres.log" -o "-h 127.0.0.1 -p $port -k $pg_root" -w start >/dev/null
    export FH2MP_TEST_PG_ADMIN_DSN="postgresql://$(id -un)@127.0.0.1:$port/postgres"
}

lane_shim() {
    # One booted prefix, copied for each check (shim/tests/wine_prefix.sh); a Wine that cannot start is reported as that, not as a failed check.
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
    for t in gamedb_files game_image game_memory title_scope xhr_config health_thread session_log torque_scale net_strings torque_load speed_pb restricted_tools lighting honk diag party_hud car_pool crash_guards picker_pi event_stock; do
        "shim/tests/test_$t.sh" > "$logs/$t.log" 2>&1 &
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

lane_go() {   # the servers (native/): format, vet, tests. The tests that need the game catalog or <repo>/data skip without them; the others use a disposable PostgreSQL
    command -v go >/dev/null 2>&1 || { echo "go lane: go is not installed" >&2; failed=1; return; }
    run "go fmt" sh -c 'cd native && test -z "$(gofmt -l .)"'
    run "go vet" sh -c 'cd native && go vet ./...'
    if pg_start; then
        run "go test" sh -c 'cd native && go test ./...'
        pg_stop
        unset FH2MP_TEST_PG_ADMIN_DSN
    else
        echo "go lane: no PostgreSQL here, the database tests are skipped" >&2
        run "go test" sh -c 'cd native && go test ./...'
    fi
}

lane_mutations() {
    run "mutations launcher" sh -c 'cd launcher && "$0" -I ../release/check_test_mutations.py --area launcher' "$py"
}

for lane in "${lanes[@]}"; do
    case "$lane" in
        quick) lane_go; lane_unit; lane_launcher ;;
        all) lane_go; lane_unit; lane_shim; lane_launcher; lane_mutations ;;
        *) "lane_$lane" ;;
    esac
done

echo
printf '%s\n' "${summary[@]}"
echo "total $(( $(date +%s) - started ))s"
exit $failed
