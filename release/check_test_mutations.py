#!/usr/bin/env python3
"""Check that important regressions are detected, without changing repo files.

Each mutation runs the unmodified tests against one deliberately broken function
in a fresh process. A passing mutant is a failure of this check. Assertion
failures count as detection; import/setup errors do not count as useful evidence.
The server cases went with the Python servers (2026-11-07); only launcher cases remain.
"""
import argparse
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    'unverified-update': ('launcher', ['test_core.UpdaterTests.test_tampered_or_foreign_manifest_rejected'],
                           'fh2mp_launcher.updater.verify_manifest', lambda raw, *args: json.loads(raw)),
    'shim-hotfix-ignored': ('launcher', ['test_shim_hotfix.LauncherShimTargetTests.test_a_shim_hotfix_makes_the_release_shim_outdated'],
                            'fh2mp_launcher.updater.shim_entry', lambda *args, **kw: None),
    'manifest-replay-accepted': ('launcher', ['test_shim_hotfix.ReplayTests.test_an_older_manifest_than_one_seen_is_refused'],
                                 'fh2mp_launcher.core.Launcher._refuse_replay', lambda self, m: None),
    'save-verification-disabled': ('launcher', ['test_saves.SaveTests.test_corrupt_copy_is_detected_before_replacement'],
                                    'fh2mp_launcher.saves._manifest', lambda path: {}),
    'memguard-never-stops-a-runaway': ('launcher', ['test_memguard.RunawayTests.test_a_fast_runaway_with_the_pc_running_out_is_ended'],
                                       'fh2mp_launcher.memguard.runaway', lambda *args, **kw: None),
    'memguard-ends-normal-play': ('launcher', ['test_memguard.RunawayTests.test_normal_play_is_left_alone',
                                               'test_memguard.RunawayTests.test_a_slow_fill_or_another_program_is_left_alone'],
                                  'fh2mp_launcher.memguard.runaway', lambda *args, **kw: (3 << 30, 1.0)),
}


def child(name, control=False):
    _area, names, target, replacement = CASES[name]
    sys.path.insert(0, str(ROOT / 'launcher/tests'))
    sys.path.insert(0, str(ROOT / 'launcher'))
    suite = unittest.defaultTestLoader.loadTestsFromNames(names)
    if unittest.defaultTestLoader.errors:
        raise RuntimeError('\n'.join(unittest.defaultTestLoader.errors))
    output = io.StringIO()
    context = mock.patch(target, replacement) if not control else contextlib.nullcontext()
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
    parser.add_argument('--area', choices=('launcher',), required=True)
    parser.add_argument('--child', choices=CASES)
    parser.add_argument('--control', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.child:
        return child(args.child, args.control)
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
