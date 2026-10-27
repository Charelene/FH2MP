#!/usr/bin/env python3
"""Collect every server test; classify prerequisites instead of omitting modules.

Run portable checks with the release dependencies installed. PostgreSQL checks
require FH2MP_TEST_PG_DSN pointing at a disposable fh2mp_test* database. Catalog
checks additionally need the user's extracted catalog; it is never shipped.
"""
import argparse
from collections import Counter
import contextlib
import io
import json
import os
from pathlib import Path
import random
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
# Classification is exhaustive: adding a test module without a decision fails
# collection. Class overrides keep pure checks in mixed modules in the gate.
PLAN = {
    **dict.fromkeys((
        'test_community', 'test_crash_reports', 'test_event_results',
        'test_freeroam_tag_grant', 'test_gameopts', 'test_home',
        'test_lobby_packing', 'test_ops', 'test_parts', 'test_phasing',
        'test_player_setup', 'test_race_end', 'test_race_flow', 'test_replay',
        'test_restricted_tools', 'test_road_trip', 'test_score_reports',
        'test_security', 'test_send_queue', 'test_site_stats', 'test_speedcheck',
        'test_tunnel', 'test_world_capacity', 'test_xp_audit', 'test_xls',
    ), 'portable'),
    'test_auth': 'postgres', 'test_gamedb_sync': 'postgres', 'test_community_pg': 'postgres',
    'test_db': 'postgres', 'test_db.DisplayLevelTests': 'portable',
    'test_db.ScratchDatabaseGuardTests': 'portable',
    'test_lsp': 'portable', 'test_lsp.LspTests': 'postgres-catalog',
    'test_rentals': 'catalog', 'test_rentals.Plan': 'portable',
}


def flatten(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from flatten(item)
        else:
            yield item


def collect():
    files = {p.stem for p in (ROOT / 'server').glob('test_*.py')}
    classified = {name for name in PLAN if '.' not in name}
    if files != classified:
        raise ValueError(f'Test plan mismatch: unclassified={sorted(files - classified)}, missing={sorted(classified - files)}')
    sys.path.insert(0, str(ROOT / 'server'))
    suite = unittest.defaultTestLoader.discover(str(ROOT / 'server'), pattern='test_*.py')
    tests = list(flatten(suite))
    if unittest.defaultTestLoader.errors:
        raise ValueError('\n'.join(unittest.defaultTestLoader.errors))
    duplicates = [name for name, count in Counter(t.id() for t in tests).items() if count > 1]
    if duplicates:
        raise ValueError(f'Tests collected more than once (usually an imported TestCase helper): {duplicates}')
    return tests


def lane(test):
    module, cls, _method = test.id().rsplit('.', 2)
    return PLAN.get(module + '.' + cls, PLAN[module])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--lane', choices=('portable', 'postgres', 'catalog', 'postgres-catalog'), default='portable')
    ap.add_argument('--shuffle-seed', type=int)
    ap.add_argument('--report', type=Path)
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()
    os.environ['FH2MP_CONTROL_PORT'] = '0'
    if args.lane.startswith('postgres'):
        dsn = os.environ.get('FH2MP_TEST_PG_DSN')
        if not dsn:
            ap.error('FH2MP_TEST_PG_DSN is required for database tests')
        os.environ['FH2MP_PG_DSN'] = dsn
        os.environ['FH2MP_ENV'] = 'test'
    tests = collect()
    selected = [t for t in tests if lane(t) == args.lane]
    if not selected:
        ap.error(f'No tests selected for {args.lane}')
    omitted = Counter(lane(t) for t in tests if lane(t) != args.lane)
    if args.shuffle_seed is not None:
        random.Random(args.shuffle_seed).shuffle(selected)
    print(f'Collected {len(tests)} server tests; selected {len(selected)} ({args.lane}).', flush=True)
    print('Not run in this lane: ' + ', '.join(f'{name}={n}' for name, n in sorted(omitted.items())), flush=True)
    if args.list:
        for test in selected:
            print(test.id())
        return 0
    # Keep server diagnostic prints out of the result summary; failure tracebacks
    # still go through unittest's stderr. Include logs when checks fail.
    logs = io.StringIO()
    with contextlib.redirect_stdout(logs):
        result = unittest.TextTestRunner(verbosity=1).run(unittest.TestSuite(selected))
    if not result.wasSuccessful():
        print(logs.getvalue(), file=sys.stderr)
    report = {
        'lane': args.lane, 'collected': len(tests), 'run': result.testsRun,
        'not_run': dict(omitted), 'failures': [t.id() for t, _ in result.failures],
        'errors': [t.id() for t, _ in result.errors],
        'skipped': [{'test': t.id(), 'reason': why} for t, why in result.skipped],
        'tests': [t.id() for t in selected], 'shuffle_seed': args.shuffle_seed,
    }
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
