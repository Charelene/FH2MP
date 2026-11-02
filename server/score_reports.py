"""Submission moderation. Runs and ghosts are never deleted; reports and decisions remain available for analysis."""
import json
import re
import time

import diag

LOG = diag.get("reports")

THRESHOLD = 10
VISIBLE = "NOT EXISTS (SELECT 1 FROM hidden_scores hs WHERE hs.submission_id = s.id)"
COUNTS = "(SELECT COUNT(*) FROM score_reports sr WHERE sr.submission_id = s.id AND sr.dismissed = 0)"
OWN_PENDING = "(s.xuid = ? AND EXISTS (SELECT 1 FROM hidden_scores hs WHERE hs.submission_id = s.id AND hs.source = 'pending'))"
REVIEW_BOARDS = '(1,3,6,9)'


def visible_to(viewer):
    """VISIBLE plus `viewer`'s own run that is waiting for its ghost: the game asks for its best right after submitting, before the upload."""
    return (f'({VISIBLE} OR {OWN_PENDING})', [viewer]) if viewer is not None else (VISIBLE, [])


def fields(row):
    keys = row.keys()
    count = row['report_count'] if 'report_count' in keys else 0
    return {'submissionId': row['id'], 'reports': count, 'suspicious': count >= THRESHOLD}


def report(lsp, submission_id, payload):
    from ops_api import OpsError
    browser, ip = payload.get('browser', ''), payload.get('ip', '')
    if not all(isinstance(v, str) and re.fullmatch(r'[0-9a-f]{64}', v) for v in (browser, ip)):
        LOG.warn(f"report on run {submission_id} refused: the reporter identity is not a 64-digit hex hash (browser is {type(browser).__name__}, ip is {type(ip).__name__}); "
                 "the website sends hashes, so this did not come from it")
        raise OpsError(400, 'bad_request', 'invalid reporter identity')
    now = time.time()
    with lsp.DB.tx() as c:
        row = c.execute(f'SELECT s.id FROM score_submissions s WHERE s.id = ? AND s.sb IN (1,3,6,9) AND {VISIBLE}', (submission_id,)).fetchone()
        if row is None:
            LOG.info(f"report on run {submission_id} refused: not on a reviewable board, hidden already, or unknown")
            raise OpsError(404, 'not_found', 'this run is no longer available')
        duplicate = c.execute('SELECT 1 FROM score_reports WHERE submission_id = ? AND (browser = ? OR ip = ?)',
                              (submission_id, browser, ip)).fetchone()
        if duplicate:
            LOG.debug(f"report on run {submission_id} ignored: this browser or address reported it already")
            return {'reported': False, 'message': 'You have already reported this run.'}
        count = c.execute('SELECT COUNT(*) FROM score_reports WHERE created >= ? AND (browser = ? OR ip = ?)',
                          (now - 3600, browser, ip)).fetchone()[0]
        if count >= 20:
            LOG.warn(f"report on run {submission_id} refused: {count} reports from this reporter in the last hour (limit 20) " + diag.kv(browser=browser[:8]))
            raise OpsError(429, 'rate_limited', 'Too many reports. Try again later.')
        c.execute('INSERT INTO score_reports (submission_id, browser, ip, created) VALUES (?,?,?,?)',
                  (submission_id, browser, ip, now))
        total = c.execute('SELECT COUNT(*) FROM score_reports WHERE submission_id = ? AND dismissed = 0', (submission_id,)).fetchone()[0]
    LOG.info(f"run {submission_id} reported " + diag.kv(open_reports=total, threshold=THRESHOLD, marked_suspicious=("now" if total == THRESHOLD else total > THRESHOLD or None),
                                                      browser=browser[:8]))
    lsp._site_history = None
    return {'reported': True, 'message': 'Run reported. Thank you.'}


def queue(lsp, limit=50, offset=0):
    """Reported runs, runs hidden by a person or a verdict, and runs a verdict flagged. A run still waiting for its ghost is not a decision."""
    from ops_api import _gamertag
    rows = lsp.DB.query(f'''SELECT s.*, COALESCE(p.gamertag, '') AS tag, {COUNTS} AS report_count,
        (SELECT hidden_at FROM hidden_scores hs WHERE hs.submission_id = s.id AND hs.source <> 'pending') AS hidden_at,
        (SELECT hs.source FROM hidden_scores hs WHERE hs.submission_id = s.id) AS hidden_by,
        v.verdict, v.reasons
        FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid LEFT JOIN score_verdicts v ON v.submission_id = s.id
        WHERE EXISTS (SELECT 1 FROM score_reports sr WHERE sr.submission_id = s.id AND sr.dismissed = 0)
        OR EXISTS (SELECT 1 FROM hidden_scores hs WHERE hs.submission_id = s.id AND hs.source <> 'pending')
        OR (v.verdict = 'flag' AND v.reviewed = 0)
        ORDER BY report_count DESC, s.id DESC LIMIT ? OFFSET ?''', (limit + 1, offset))
    import rivals
    names = lsp.RIV.names()['car']
    out = [{'submissionId': r['id'], 'gamertag': _gamertag(lsp, r), 'xuid': r['xuid'], 'score': r['score'],
            'unit': 's' if r['st'] == 1 else 'm/s', 'car': names.get(r['car'], ''), 'class': rivals.CLASS_LETTERS[r['cls']] if 0 <= r['cls'] < len(rivals.CLASS_LETTERS) else str(r['cls']),
            'boardType': r['sb'], 'track': r['track'], 'event': r['gsid'], 'at': int(r['submitted']),
            'reports': r['report_count'], 'hidden': r['hidden_at'] is not None, 'hiddenBy': r['hidden_by'] if r['hidden_at'] is not None else None,
            'verdict': r['verdict'], 'reasons': json.loads(r['reasons']) if r['reasons'] else []} for r in rows[:limit]]
    return {'entries': out, 'hasNext': len(rows) > limit}


def moderate(lsp, submission_id, action):
    from ops_api import OpsError
    if action not in ('hide', 'dismiss', 'unhide'):
        LOG.warn(f"moderation of run {submission_id} refused: unknown action {str(action)[:20]!r}, expected hide, dismiss or unhide")
        raise OpsError(400, 'bad_request', 'unknown moderation action')
    now = time.time()
    with lsp.DB.tx() as c:
        if c.execute(f'SELECT id FROM score_submissions WHERE id = ? AND sb IN {REVIEW_BOARDS}', (submission_id,)).fetchone() is None:
            LOG.warn(f"moderation {action} of run {submission_id} refused: no such run on a reviewable board {REVIEW_BOARDS}")
            raise OpsError(404, 'not_found', 'unknown run')
        if action == 'hide':
            c.execute("INSERT INTO hidden_scores (submission_id, hidden_at, source) VALUES (?,?,'operator') ON CONFLICT (submission_id) "
                      "DO UPDATE SET source = 'operator'", (submission_id, now))
            c.execute('DELETE FROM score_overrides WHERE submission_id = ?', (submission_id,))
        elif action == 'unhide':
            c.execute('DELETE FROM hidden_scores WHERE submission_id = ?', (submission_id,))
            c.execute('INSERT INTO score_overrides (submission_id, at) VALUES (?,?) ON CONFLICT (submission_id) DO NOTHING', (submission_id, now))
            c.execute('UPDATE score_verdicts SET reviewed = 1 WHERE submission_id = ?', (submission_id,))
        else:
            c.execute('UPDATE score_reports SET dismissed = 1 WHERE submission_id = ?', (submission_id,))
            c.execute('UPDATE score_verdicts SET reviewed = 1 WHERE submission_id = ?', (submission_id,))
    LOG.info(f"operator {action} of run {submission_id}")
    lsp._site_history = None
    return {'ok': True}
