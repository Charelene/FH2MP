"""Submission moderation. Runs and ghosts are never deleted; reports and decisions remain available for analysis."""
import re
import time

THRESHOLD = 10
VISIBLE = "NOT EXISTS (SELECT 1 FROM hidden_scores hs WHERE hs.submission_id = s.id)"
COUNTS = "(SELECT COUNT(*) FROM score_reports sr WHERE sr.submission_id = s.id AND sr.dismissed = 0)"


def fields(row):
    keys = row.keys()
    count = row['report_count'] if 'report_count' in keys else 0
    return {'submissionId': row['id'], 'reports': count, 'suspicious': count >= THRESHOLD}


def report(lsp, submission_id, payload):
    from ops_api import OpsError
    browser, ip = payload.get('browser', ''), payload.get('ip', '')
    if not all(isinstance(v, str) and re.fullmatch(r'[0-9a-f]{64}', v) for v in (browser, ip)):
        raise OpsError(400, 'bad_request', 'invalid reporter identity')
    now = time.time()
    with lsp.DB.tx() as c:
        row = c.execute(f'SELECT s.id FROM score_submissions s WHERE s.id = ? AND s.sb IN (1,3,9) AND {VISIBLE}', (submission_id,)).fetchone()
        if row is None:
            raise OpsError(404, 'not_found', 'this run is no longer available')
        duplicate = c.execute('SELECT 1 FROM score_reports WHERE submission_id = ? AND (browser = ? OR ip = ?)',
                              (submission_id, browser, ip)).fetchone()
        if duplicate:
            return {'reported': False, 'message': 'You have already reported this run.'}
        count = c.execute('SELECT COUNT(*) FROM score_reports WHERE created >= ? AND (browser = ? OR ip = ?)',
                          (now - 3600, browser, ip)).fetchone()[0]
        if count >= 20:
            raise OpsError(429, 'rate_limited', 'Too many reports. Try again later.')
        c.execute('INSERT INTO score_reports (submission_id, browser, ip, created) VALUES (?,?,?,?)',
                  (submission_id, browser, ip, now))
    lsp._site_history = None
    return {'reported': True, 'message': 'Run reported. Thank you.'}


def queue(lsp, limit=50, offset=0):
    from ops_api import _gamertag
    rows = lsp.DB.query(f'''SELECT s.*, COALESCE(p.gamertag, '') AS tag, {COUNTS} AS report_count,
        (SELECT hidden_at FROM hidden_scores hs WHERE hs.submission_id = s.id) AS hidden_at
        FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid
        WHERE EXISTS (SELECT 1 FROM score_reports sr WHERE sr.submission_id = s.id AND sr.dismissed = 0)
        OR EXISTS (SELECT 1 FROM hidden_scores hs WHERE hs.submission_id = s.id)
        ORDER BY report_count DESC, s.id DESC LIMIT ? OFFSET ?''', (limit + 1, offset))
    import rivals
    names = lsp.RIV.names()['car']
    out = [{'submissionId': r['id'], 'gamertag': _gamertag(lsp, r), 'xuid': r['xuid'], 'score': r['score'],
            'unit': 's' if r['st'] == 1 else 'm/s', 'car': names.get(r['car'], ''), 'class': rivals.CLASS_LETTERS[r['cls']] if 0 <= r['cls'] < len(rivals.CLASS_LETTERS) else str(r['cls']),
            'boardType': r['sb'], 'track': r['track'], 'event': r['gsid'], 'at': int(r['submitted']),
            'reports': r['report_count'], 'hidden': r['hidden_at'] is not None} for r in rows[:limit]]
    return {'entries': out, 'hasNext': len(rows) > limit}


def moderate(lsp, submission_id, action):
    from ops_api import OpsError
    if action not in ('hide', 'dismiss'):
        raise OpsError(400, 'bad_request', 'unknown moderation action')
    with lsp.DB.tx() as c:
        if c.execute('SELECT id FROM score_submissions WHERE id = ? AND sb IN (1,3,9)', (submission_id,)).fetchone() is None:
            raise OpsError(404, 'not_found', 'unknown run')
        if action == 'hide':
            c.execute('INSERT INTO hidden_scores (submission_id, hidden_at) VALUES (?,?) ON CONFLICT (submission_id) DO NOTHING',
                      (submission_id, time.time()))
        else:
            c.execute('UPDATE score_reports SET dismissed = 1 WHERE submission_id = ?', (submission_id,))
    lsp._site_history = None
    return {'ok': True}
