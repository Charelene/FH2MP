#!/usr/bin/env python3
"""Check that important regressions are detected, without changing repo files.

Each mutation runs the unmodified tests against one deliberately broken function
in a fresh process. A passing mutant is a failure of this check. Assertion
failures count as detection; import/setup errors do not count as useful evidence.
"""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    'lost-host-settings': ('server', ['test_race_flow.RaceFlow.test_returning_host_snapshot_is_delivered_before_invite'],
                           'fh2mp_server.Server.accept_requested_options', lambda self, peer: None),
    'no-options-delivery': ('server', ['test_race_flow.RaceFlow.test_invite_carries_host_class_and_both_clients_reach_go'],
                            'fh2mp_server.Server.send_game_options', lambda *args, **kw: None),
    'no-class-enforcement': ('server', ['test_race_flow.RaceFlow.test_wrong_class_is_refused_then_a_valid_selection_can_join'],
                             'gameopts.accepts_car', lambda *args: True),
    'ignored-sparse-car-pick': ('server', ['test_race_flow.RaceFlow.test_car_choice_in_a_mixed_delta_is_used_for_admission'],
                                'fh2mp_server.Server.note_typed_choices', lambda self, peer, size, payload: payload),
    'no-grid-delivery': ('server', ['test_race_flow.RaceFlow.test_invite_carries_host_class_and_both_clients_reach_go'],
                         'fh2mp_server.Server.maybe_begin_race', lambda *args: None),
    'no-go-delivery': ('server', ['test_race_flow.RaceFlow.test_invite_carries_host_class_and_both_clients_reach_go'],
                       'fh2mp_server.Server.go_race', lambda *args: None),
    'unverified-update': ('launcher', ['test_core.UpdaterTests.test_tampered_or_foreign_manifest_rejected'],
                           'fh2mp_launcher.updater.verify_manifest', lambda raw, *args: json.loads(raw)),
    'save-verification-disabled': ('launcher', ['test_saves.SaveTests.test_corrupt_copy_is_detected_before_replacement'],
                                    'fh2mp_launcher.saves._manifest', lambda path: {}),
    'xp-cheats-counted': ('server', ['test_xp_audit.Judge.test_impossible_awards_are_rejected'],
                           'xp_audit.judge', lambda xp: ('ok', '')),
    'speed-enforcement-disabled': ('server', ['test_speedcheck.SpeedCheck.test_climbing_for_three_seconds_is_flying'],
                                    'fh2mp_server.Server.track_speed', lambda *args: None),
    'logout-not-revoked': ('postgres', ['test_auth.HttpTests.test_actual_launcher_registration_login_recovery_and_logout'],
                            'auth.AuthService.logout', lambda *args: None),
}


def child(name, baseline=None, control=False):
    area, names, target, replacement = CASES[name]
    sys.path.insert(0, str(ROOT / ('launcher/tests' if area == 'launcher' else 'server')))
    if area == 'launcher':
        sys.path.insert(0, str(ROOT / 'launcher'))
    if baseline:
        spec = importlib.util.spec_from_file_location('fh2mp_server', baseline)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
    suite = unittest.defaultTestLoader.loadTestsFromNames(names)
    if unittest.defaultTestLoader.errors:
        raise RuntimeError('\n'.join(unittest.defaultTestLoader.errors))
    output = io.StringIO()
    context = mock.patch(target, replacement) if not baseline and not control else contextlib.nullcontext()
    with contextlib.redirect_stdout(output), context:
        result = unittest.TextTestRunner(stream=output).run(suite)
    if control:
        if result.wasSuccessful() and result.testsRun and not result.skipped:
            return 0
        print(output.getvalue())
        print(f'CONTROL FAILED {name}; mutation detection would be meaningless')
        return 1
    if result.failures and not result.errors and not result.skipped:
        print(f'DETECTED {name}: ' + ', '.join(t.id() for t, _ in result.failures))
        return 0
    print(output.getvalue())
    print(f'NOT PROVEN {name}: failures={len(result.failures)}, errors={len(result.errors)}')
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--area', choices=('server', 'launcher', 'postgres'), required=True)
    parser.add_argument('--child', choices=CASES)
    parser.add_argument('--control', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--baseline', type=Path, help='optional prior server source for a local regression check')
    args = parser.parse_args()
    if args.baseline and (args.area != 'server' or not args.child):
        parser.error('--baseline requires --area server and one --child regression')
    if args.child:
        return child(args.child, args.baseline, args.control)
    failed = False
    for name, (area, _names, _target, _replacement) in CASES.items():
        if area != args.area:
            continue
        command = [sys.executable, '-I', __file__, '--area', area, '--child', name]
        control = subprocess.run(command + ['--control'], capture_output=True, text=True, cwd=ROOT, timeout=45)
        if control.returncode:
            print((control.stdout + control.stderr).strip())
            failed = True
            continue
        result = subprocess.run(command, capture_output=True, text=True, cwd=ROOT, timeout=45)
        print((result.stdout + result.stderr).strip())
        failed |= result.returncode != 0
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
