"""Production release orchestration against a temporary local Git remote.

GitHub, SSH, publication and deployment are stand-ins. No network or real
service is used; assertions cover refusal boundaries and tag/commit selection.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseFlow(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / 'repo'
        self.repo.mkdir()
        self.bin = self.base / 'bin'
        self.bin.mkdir()
        self.trace = self.base / 'trace'
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ['PATH'],
                        TRACE=str(self.trace), FH2MP_CONFIRM_TAG='v1.1.0',
                        FH2MP_RELEASE_KEY_FILE=str(self.base / 'key'))
        (self.base / 'key').write_text('test fixture, not a signing key')
        self.write('shim/VERSION', '1.1.0\n')
        self.write('launcher/fh2mp_launcher/version.py', '__version__ = "1.1.0"\n')
        self.write('release/notes/v1.1.0.md', 'Release fixture\n')
        for name in ('ship.sh',):
            self.write('ops/' + name, (ROOT / 'ops' / name).read_text(), executable=True)
        self.write('release/publish.sh', '#!/bin/sh\necho publish >> "$TRACE"\n', executable=True)
        self.write('ops/release-prod.sh', '#!/bin/sh\nprintf "deploy %s\\n" "$*" >> "$TRACE"\n', executable=True)
        self.tool('gh', '''case "$1 $2" in
  "pr list") echo 7;;
  "pr merge") git push -q origin HEAD:refs/heads/main;;
  "run list") echo 42;;
  "run watch") [ "${BUILD_FAIL:-0}" = 0 ];;
  "release view") [ "${NO_MANIFEST:-0}" = 1 ] || echo manifest.json;;
esac
''')
        self.tool('ssh', 'cat >/dev/null\necho ssh >> "$TRACE"\n')
        self.tool('sleep', 'exit 0\n')
        self.git('init', '-q', '-b', 'develop')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('config', 'user.name', 'Release fixture')
        self.git('add', '.')
        self.git('commit', '-qm', 'Reviewed fixture')
        self.git('branch', 'main')
        subprocess.run(['git', 'init', '-q', '--bare', str(self.base / 'remote.git')], check=True)
        self.git('remote', 'add', 'origin', str(self.base / 'remote.git'))
        self.git('push', '-q', 'origin', 'develop', 'main')

    def write(self, name, text, executable=False):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        if executable:
            path.chmod(0o755)

    def tool(self, name, text):
        path = self.bin / name
        path.write_text('#!/bin/sh\nset -e\n' + text)
        path.chmod(0o755)

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.repo, text=True).strip()

    def run_ship(self, execute=True, **env):
        return subprocess.run(['bash', 'ops/ship.sh', 'v1.1.0', 'deploy@example.invalid', str(self.base / 'key')]
                              + (['--execute'] if execute else []), cwd=self.repo,
                              env=dict(self.env, **env), capture_output=True, text=True, timeout=20)

    def test_plan_makes_no_changes_even_with_unreviewed_files(self):
        self.write('scratch.txt', 'not for release')
        before = self.git('status', '--porcelain')
        result = self.run_ship(execute=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git('status', '--porcelain'), before)
        self.assertFalse(self.trace.exists())
        self.assertEqual(self.git('tag', '--list'), '')

    def test_execution_refuses_dirty_tree_without_staging(self):
        self.write('scratch.txt', 'not for release')
        result = self.run_ship()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.trace.exists())
        self.assertEqual(self.git('diff', '--cached', '--name-only'), '')
        self.assertEqual(self.git('tag', '--list'), '')

    def test_failed_build_cannot_deploy_or_clear_maintenance(self):
        result = self.run_ship(BUILD_FAIL='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.trace.read_text().strip(), 'publish')

    def test_missing_manifest_cannot_deploy(self):
        result = self.run_ship(NO_MANIFEST='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.trace.read_text().strip(), 'publish')

    def test_success_tags_reviewed_commit_then_deploys_both_services(self):
        head = self.git('rev-parse', 'HEAD')
        result = self.run_ship()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.git('rev-parse', 'v1.1.0^{commit}'), head)
        self.assertEqual(self.git('branch', '--show-current'), 'develop')
        lines = self.trace.read_text().splitlines()
        self.assertEqual(lines[0], 'publish')
        self.assertIn('deploy v1.1.0 deploy@example.invalid', lines[1])
        self.assertNotIn('--skip-game', lines[1])
        self.assertEqual(lines[2], 'ssh')

    def test_publisher_refuses_dirty_tree_and_preserves_existing_index(self):
        shutil.copyfile(ROOT / 'release/publish.sh', self.repo / 'release/publish.sh')
        self.git('add', 'release/publish.sh')
        self.git('commit', '-qm', 'Use actual publisher')
        self.write('reviewed.txt', 'staged intentionally')
        self.git('add', 'reviewed.txt')
        self.write('scratch.txt', 'never stage automatically')
        before = self.git('diff', '--cached')
        result = subprocess.run(['bash', 'release/publish.sh', 'v1.1.0'], cwd=self.repo,
                                env=self.env, capture_output=True, text=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.git('diff', '--cached'), before)
        self.assertIn('?? scratch.txt', self.git('status', '--porcelain'))

    def test_publisher_tags_only_a_commit_whose_ci_run_passed(self):
        shutil.copyfile(ROOT / 'release/publish.sh', self.repo / 'release/publish.sh')
        self.git('add', 'release/publish.sh')
        self.git('commit', '-qm', 'Use actual publisher')
        head = self.git('rev-parse', 'HEAD')
        self.tool('gh', '''echo "gh $*" >> "$TRACE"
case "$1 $2" in
  "auth status") ;;
  "run list") case "$*" in *"--workflow ci.yml"*"--commit $CI_COMMIT"*) echo 42;; esac;;
  "run watch") [ "${CI_FAIL:-0}" = 0 ];;
  "secret set") cat >/dev/null;;
esac
''')
        publish = lambda **env: subprocess.run(['bash', 'release/publish.sh', 'v1.1.0'], cwd=self.repo, capture_output=True,
                                               text=True, timeout=20, env=dict(self.env, CI_COMMIT=head, **env))
        failed = publish(CI_FAIL='1')
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn('checks failed', failed.stdout)
        trace = self.trace.read_text()
        self.assertNotIn('secret set', trace)
        self.assertEqual(self.git('tag', '--list'), '')
        self.trace.write_text('')
        passed = publish()
        self.assertEqual(passed.returncode, 0, passed.stdout + passed.stderr)
        self.assertEqual(self.git('rev-parse', 'v1.1.0^{commit}'), head)
        trace = self.trace.read_text()
        self.assertLess(trace.index('run watch'), trace.index('secret set'))


class VersionedEntryPoint(unittest.TestCase):
    def test_saved_target_supports_no_argument_plan_and_explicit_execution(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'release/notes').mkdir(parents=True)
            (root / 'ops').mkdir()
            wrapper = root / 'release/notes/v1.1.1.deploy.sh'
            shutil.copyfile(ROOT / 'release/notes/v1.1.1.deploy.sh', wrapper)
            (root / 'release/operator.local.env').write_text(
                'FH2MP_DEPLOY_HOST=deploy@example.invalid\nFH2MP_DEPLOY_KEY=/test/key with spaces\n')
            ship = root / 'ops/ship.sh'
            ship.write_text('#!/bin/sh\nprintf "<%s>\\n" "$@"\n')
            ship.chmod(0o755)
            env = dict(os.environ)
            env.pop('FH2MP_DEPLOY_HOST', None)
            env.pop('FH2MP_DEPLOY_KEY', None)
            for args in ([], ['--execute']):
                result = subprocess.run(['bash', str(wrapper), *args], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(),
                                 ['<v1.1.1>', '<deploy@example.invalid>', '</test/key with spaces>'] +
                                 (['<--execute>'] if args else []))
            (root / 'release/operator.local.env').unlink()
            missing = subprocess.run(['bash', str(wrapper)], env=env, capture_output=True, text=True)
            self.assertEqual(missing.returncode, 2)
            self.assertIn('Usage:', missing.stderr)
            self.assertNotIn('parameter null or not set', missing.stderr)


if __name__ == '__main__':
    unittest.main()
