# Test audit — 2026-10-26

Scope: every project-owned test module in `server/` and `launcher/tests/`, all
seven `shim/test_*.sh` harnesses, release test selection, packaging self-test and
the two auxiliary build/signing workflows. Third-party dependency tests are
outside this inventory. The local load generator was inspected, not executed;
it is not a regression test and its generated traffic is inappropriate for the
manual gameplay investigation required by this project.

## Findings and corrections

1. **Release checks omitted existing suites.** Road Trip, replay, speed checks,
   event results, home data, score moderation and rental planning were absent
   from the release list. PostgreSQL account/storage tests were also absent.
   `run_server_tests.py` now collects every server module, classifies external
   prerequisites, rejects unclassified modules, duplicate test IDs and empty
   selections, and prints which prerequisite lanes were not run. Both local
   publishing and release CI use it. A required PostgreSQL job runs the real
   migrations, account/API tests and community store contracts.
2. **Race tests bypassed the failure path.** Fixtures assigned `Peer.choices`
   and called `start_event` directly. Checking `game_options`/`gopts_view` did
   not prove a guest received the restriction before its invitation. The new
   `test_race_flow.py` uses handshake, entry, ownership, typed player updates,
   readiness, timers, loading, grid, GO and finishing through `Server.handle`.
   An independent decoder inspects flushed datagrams. No production packet
   codec generates its expectations; no real sockets or game clients are used.
3. **Those tests found additional implementation defects.** Mixed sparse class/
   PI updates were ignored, rejecting a valid car or admitting a changed car
   through loading. `note_typed_choices` now reads the typed fields in all legal
   player deltas. A setup-delay scenario also rejected the earlier local fix's
   arbitrary three-second settings expiry. That expiry and the test endorsing
   it were removed. Selecting a class and waiting before pressing Start retains
   the selection. The earlier host-handover correction remains local.
4. **Save corruption had a demonstrated false pass.** Disabling `_manifest`
   still passed the old corruption test: the transfer failed before corruption
   was injected. Tests now require reaching the fault, check the verification
   failure and compare actual file bytes with an independent filesystem reader.
5. **The launcher E2E test normally skipped, and logout checked an empty token.**
   The PostgreSQL HTTP suite now runs that existing launcher contract against a
   real local HTTP handler/store, without proprietary catalog bootstrap. Logout
   checks the saved issued token after sign-out, rather than the cleared client
   variable. Environment changes and HTTP/socket fixtures are cleaned up.
6. **Database fixtures had stale assumptions and cross-test state.** A duplicate
   rename used a now-invalid underscore; the wristband distribution fixture had
   three bands but required ten; XP ledger assertions assumed the old unscaled
   cap. These scenarios now use valid duplicate names, a 25-band distribution
   fixture scoped to that test, and an explicit cap. An additional policy test
   checks scale 1, scale 10 and an explicit override against numeric expectations.
   LSP tests now reset their mutable stores per scenario. Livery sharing logs on
   before checking durable creator searches, rather than relying on a previous
   test to have created the player.
7. **Database reset was authorized by the ordinary service DSN.** Tests now
   require `FH2MP_TEST_PG_DSN` and a resolved `fh2mp_test*` database name before
   dropping any schema. Three portable negative tests verify this guard. CI
   creates its own UTF-8 cluster on a private Unix socket. The local audit used
   a verified official PostgreSQL 16.6 source build entirely under `/tmp`.
8. **Some protocol expectations shared their implementation's oracle.** Added
   literal-byte Xls contracts for numeric endian/widths, UTF-8 byte counts,
   blobs/GUIDs, nested structs and arrays, including every truncated prefix and
   capacity/length errors. These complement the catalog-driven handler round
   trips, which cannot independently establish serialization correctness.
9. **Test isolation was incomplete.** A security TestCase imported as a helper
   was discovered twice; its import is now module-qualified and duplicate IDs
   fail collection. TCP fixtures now stop their actual Hub loop and close their
   sockets, rather than leave daemon servers running into other tests. The Hub's
   optional stop event leaves normal service behavior unchanged. Launcher test
   cache writes are directed into temporary folders. The launcher's parent log
   handle now closes after process creation, including failure paths.
10. **Inflate tests accepted output without checking process success.** All
    positive cases now require a successful subprocess exit, including empty
    output. Added invalid block/length/distance/truncation cases and output-size
    mismatches with guard bytes around the output. A crash or guard corruption
    cannot count as a safe rejection. The harness now covers 168 cases and is
    included in release checks.

## Failure sensitivity

`check_test_mutations.py` runs a passing control and then one deliberate
regression in separate processes. A control failure, skip or import/setup error
does not count as detection. All eleven checked regressions are detected:

| Deliberate regression | Observable check |
| --- | --- |
| Ignore returning host's settings | Guest receives wrong event/class before invite |
| Stop settings delivery | Invite has no matching class restriction |
| Accept every class | S1/S2/X readiness accepted for an A event |
| Ignore mixed sparse car updates | A valid changed car remains rejected |
| Suppress the starting grid | No grid reaches the selected clients |
| Suppress GO | No common race-start time reaches the clients |
| Accept impossible XP | Rejected awards become accepted |
| Disable flight/speed tracking | Sustained flying is not flagged |
| Bypass manifest signatures | Tampered/foreign manifests are accepted |
| Disable save verification | Injected corruption is not checked |
| Leave logged-out tokens valid | Real launcher E2E can still use the issued token |

The returning-host and sparse-car-selection contracts also fail against the
actual `v1.0.2` server source loaded in an isolated test process. That is separate
from deliberately mutating the current implementation. No historical game
traffic was replayed to a running client.

## Per-suite assessment

`P` = portable server lane, `DB` = disposable PostgreSQL, `C` = local catalog,
`DB+C` = disposable PostgreSQL with the user's catalog. A unit fixture is useful
for its stated boundary; direct state construction does not establish a complete
client/server lifecycle.

| Server suite | Boundary reviewed and disposition |
| --- | --- |
| `test_auth` | DB: signatures, registration, expiry, bans, renames, real HTTP identity and launcher recovery/logout. Added E2E and corrected fixture. |
| `test_community` | P: social/lease behavior through real stores/matcher using SQLite and mocked identity transport. |
| `test_community_pg` | DB: the same eleven social contracts on actual PostgreSQL migrations/constraints. |
| `test_crash_reports` | P: input limits, private retention, redaction, deduplication, download authorization; external GitHub is deliberately fake. |
| `test_db` | DB: migrations, transactions, scores, XP, notifications/concurrency and Drivatars; portable level/reset-guard checks. Corrected fixtures. |
| `test_event_results` | P: result aggregation; manually seeded race state is a unit fixture, not event setup coverage. Now included in release. |
| `test_freeroam_tag_grant` | P: tag-mode/options and ownership serializer units; native mode recognition is based on retained captures. |
| `test_gamedb_sync` | DB: authenticated manifests/downloads, replacement, retention and rate limiting on real stores. |
| `test_gameopts` | P: typed delta/lap/restriction units and focused server cases. Renamed a purported late-join test that only used an existing guest. |
| `test_home` | P: feed/cache/fallback/asset-path units with external fetches stubbed; now in release. |
| `test_lobby_packing` | P: placement/reservation/pool contracts; fake populations don't establish a gameplay connection. |
| `test_lsp` | P and DB+C: handler validation plus actual catalog serialization/storage flows. Removed shared mutable database state. |
| `test_ops` | P: HTTP/operator authorization and query/output behavior on SQLite fixture stores; not proof of PostgreSQL SQL compatibility. |
| `test_parts` | P: legacy part codec plus server policy/rental cases. Replaced a data-dependent skip with a valid wrong-table fixture. |
| `test_phasing` | P: emitted prediction/collision/score isolation; actual native scene acceptance remains outside this receiver model. |
| `test_player_setup` | P: captured malformed setups, native schema widths and before-relay/owner repairs; native rebuilding/physics is not simulated. |
| `test_race_end` | P: finish/countdown units; updated obsolete description about lap decoding. |
| `test_race_flow` | P: eleven independently decoded delivery/lifecycle contracts, alternating hosts, long sessions/wrap, late join, sparse updates and setup delay. |
| `test_rentals` | P/C: deterministic rental planning and local catalog/tool-output checks; catalog remains unredistributed. |
| `test_replay` | P: late-join compaction/continuity/deletes across long sequences; now in release. |
| `test_restricted_tools` | P: authenticated reports, acknowledgement failure and pool isolation; detector attestation/evasion is not claimed. |
| `test_road_trip` | P: database candidate selection, team/countdown/drive state units; now in release. No complete native Road Trip selector model. |
| `test_score_reports` | P: moderation thresholds, visibility and persistence on SQLite store fixtures; now in release. |
| `test_security` | P: transport/input/identity boundaries with negative controls; fake Hub fixture now closes replaced sockets. |
| `test_send_queue` | P: flushed datagrams, size/hold/ack/recipient behavior; matching server builders are supplemented by the independent race reader. |
| `test_site_stats` | P: statistics/query output on SQLite stores; live polling and actual website rendering are separate boundaries. |
| `test_speedcheck` | P: position/time-policy units with a controlled clock; not a physics simulation. Now in release and mutation-checked. |
| `test_tunnel` | P: real loopback framing/limits/Hub transport. Actual serving loop now terminates and fixtures close. |
| `test_world_capacity` | P: handshake/owner allocation/reconnect/packing; duplicate imported test removed. Native table size evidence is separate from this test. |
| `test_xls` | P: literal-byte serialization and malformed-input contracts, independent of round-trip expectations. |
| `test_xp_audit` | P: policy/log attribution; added explicit scale/cap numeric cases and a mutation check. |

| Launcher suite | Boundary reviewed and disposition |
| --- | --- |
| `test_community` | Session/token reuse and mutation retries with mocked API; underlying HTTP/auth is covered by the DB lane. |
| `test_community_ui` | Offscreen Qt interaction/focus/animation/layout tests present in the final working tree; real service calls are stubbed. |
| `test_core` | Identity/update signatures, archives, installation/backup/env/profile rules. Temporary cache isolation fixed; real Azimuth recognition run separately. |
| `test_crashreport` | Session correlation, attachment choice/redaction/size limits with real temporary files. No native dump unwinding is claimed. |
| `test_e2e` | Actual launcher/HTTP/PostgreSQL registration, recovery, token replacement and logout. Now executed by the DB lane; false logout assertion corrected. |
| `test_gamedb` | Actual downloaded/hash/replacement file behavior with fake transport; opaque test payload is not a game catalog validation. |
| `test_offline_mode` | Launcher decisions/session clearing/mod toggles with network/spawn boundaries mocked. No running game is controlled. |
| `test_saves` | Real filesystem backups, integrity, replacement and rollback; independent byte oracle and reached-fault assertion added. Elevation helpers are mocked. |
| `test_sfx` | Notification watermarks/context/deduplication and WAV shape; actual audio output isn't exercised. Included in final discovery. |
| `test_tunnel` | Real isolated TCP/UDP framing/reconnect; listener/connections/workers now close. |

| Shim harness | Boundary reviewed and disposition |
| --- | --- |
| `test_sha256.sh` | Production SHA/HMAC compared with independent Python/system implementations, block/padding boundaries. |
| `test_inflate.sh` | Production inflate against zlib; process status, negative corpus and output guards added. |
| `test_gamedb_keep.sh` | Production classification/backup rules with actual SQLite headers and state fixtures. |
| `test_gamedb_files.sh` | Actual production functions and Win32 locking/copy/restore in an isolated Wine prefix. |
| `test_game_image.sh` | Actual safe-header helper against allocated/unmapped/protected pages and the Wine reserved-read regression. |
| `test_title_scope.sh` (added after audit) | Windows title parsing, selected-title overrides, executable identity and short-file rejection. Loads a DLL built from the actual shim and verifies another selected title cannot register FH2MP in online or offline-account mode. This does not simulate another running game or title switching inside an existing launcher process. |
| `test_torque_scale.sh` | Production guard with a stand-in original function; tests null-row combinations and releases, not actual game torque. |
| `test_net_strings.sh` (added after audit) | Production preflight and all three receive wrappers, offline codec fixtures across all bit alignments/length encodings, every truncation of each valid fixture, first/second-field boundary limits, literal length 1090, protected/freed memory, unchanged native cursor and no delegate on rejection. 193,940 checks pass; disabling receive preflight makes the harness fail. Native live delivery and packet-origin attribution remain unverified. |
| `test_torque_load.sh` (added after audit) | Production preloader guard with native DB/resolver/preloader stand-ins: 528 Windows/Wine checks for counts0–511/UINT_MAX, missing/empty/wrong-owner rows, native stock/electric controls, reference balance, fallback/cancellation before physics and suppression of rejected backup physics. Count-check and backup-sentinel mutations each fail. Native preloader/worker/restoration paths traced read-only in 1.0.0.64; actual game recovery and hook registration unverified. |
| `test_platform_scanner.sh` (manual diagnostic) | Original hash-checked upstream2.7.2 DLL in an isolated Wine host, with neither game nor FH2MP loaded. Paired committed/revoked private-page controls invoke unchanged original scanner; expected18FA AV verified by instruction/read address/query count. Process-local VirtualQuery import instrumentation only; on-disk hashes preserved. Requires the user's official upstream DLL, so not a CI dependency. Does not measure FH2MP timing influence in real launches. |
| `test_speed_pb.sh` (added after audit) | Production PB hooks with native save/HUD stand-ins and real Windows TLS; old-behavior control, trap/zone slower/tied/improved runs, delayed/nested/concurrent comparisons, weak lifetime, aborted zones and bounded eviction. Native offsets and actual notification rendering still require manual in-game verification. |
| `test_restricted_tools.sh` | Actual process detection/platform registration with a temporary helper in its own Wine prefix. |
| `test_lighting.sh` | Isolated Windows codec/HMAC vector, replay/epoch/length/owner rejection, remote identity matching, stale-state fallback and scoped visual-field restoration. No game loaded or network traffic to clients; real beam/material/pop-up visibility requires manual two-client checks. |

The packaging `--selftest` is an import smoke check. The auxiliary test-build
workflow builds an artifact, and sign-test validates Authenticode when run with
its configured service. Neither establishes multiplayer behavior. Those cloud
workflows/signing were inspected but not triggered during this audit.

## Validation and remaining boundaries

The final inventory contains 545 unique server tests: 435 portable, 91 PostgreSQL,
14 PostgreSQL/catalog and five local-catalog tests. All prerequisite lanes were
run locally; the portable lane was also run in shuffled order. Launcher discovery
ran 149 tests, normally skipping external-server E2E and real-install recognition;
the same E2E contract was executed against the disposable HTTP/DB fixture, and
recognition was separately run read-only against the existing test rig. All seven
shim harnesses ran, including Win32 tests in isolated temporary Wine prefixes.

Eleven deliberate regressions are detected after passing controls. These are
selected failure-sensitivity checks, not a claim that every possible regression
will be detected. The saved local logs under `/tmp/fh2mp-*-audit*.log` and
`/tmp/fh2mp-*-mutations*.log` contain the execution results.

Actual native car-selector consumption, car/physics loading, native Windows
UAC, and full Road Trip gameplay remain separate boundaries. No game input,
invented state or synthetic traffic was sent to a running game. No production or
dev service was deployed/restarted, and no release/signing workflow was run.
Windows/Linux release CI changes are prepared locally; no remote Actions run is
claimed. The PostgreSQL/catalog lane cannot be put in public CI without the
user-owned catalog, so it remains an explicit local prerequisite lane.

The earlier production log also contains an A-class selection corrected to its
model's stock B class by the rental path. Current rental tests establish stock
placeholder repair, not native event auto-tuning. That remains a specific
investigation gap; this audit does not claim every observed class mismatch or
loading loop is fixed.

The LSP default-parameter smoke test explicitly excludes `NotifyRival`, which
requires a real notification-shaped request. Its name/docstring now state that
exception; it must not be described as proof that every handler was exercised.

Reproduce the automated checks with the pinned test dependencies installed:

```sh
.venv-pg/bin/python -I release/run_server_tests.py
.venv-pg/bin/python -I release/run_server_tests.py --shuffle-seed 251026
# Set FH2MP_TEST_PG_DSN to your disposable fh2mp_test* database first:
.venv-pg/bin/python -I release/run_server_tests.py --lane postgres
.venv-pg/bin/python -I release/run_server_tests.py --lane catalog
.venv-pg/bin/python -I release/run_server_tests.py --lane postgres-catalog
.venv-pg/bin/python -I release/check_test_mutations.py --area server
.venv-pg/bin/python -I release/check_test_mutations.py --area postgres
launcher/.venv/bin/python -I release/check_test_mutations.py --area launcher
```
