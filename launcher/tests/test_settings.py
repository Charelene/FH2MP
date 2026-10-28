"""Developer endpoint migration with existing profiles and explicit overrides."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fh2mp_launcher import settings


class DevEndpointMigration(unittest.TestCase):
    def test_legacy_default_migrates_once_and_new_tunnel_choice_survives(self):
        cfg = {'api_base': 'https://dev-api.forza.scuffed.af', 'migrate_legacy_dev_api': True}
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, {'FH2MP_DEV': '1'}), mock.patch.object(settings.buildcfg, 'get', side_effect=lambda key, default=None: cfg.get(key, default)):
            with mock.patch.dict(os.environ):
                os.environ.pop('FH2MP_API', None)
                path = Path(tmp) / 'settings.json'
                path.write_text(json.dumps({'api_base': 'http://127.0.0.1:5180', 'azimuth_dir': '/my/game'}))
                migrated = settings.Settings(str(path))
                self.assertEqual(migrated['api_base'], cfg['api_base'])
                self.assertEqual(migrated['azimuth_dir'], '/my/game')
                migrated['api_base'] = 'http://127.0.0.1:5180'
                migrated.save()
                self.assertEqual(settings.Settings(str(path))['api_base'], 'http://127.0.0.1:5180')

    def test_custom_endpoint_stable_profile_and_env_override_are_preserved(self):
        cfg = {'api_base': 'https://dev-api.forza.scuffed.af', 'migrate_legacy_dev_api': True}
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(settings.buildcfg, 'get', side_effect=lambda key, default=None: cfg.get(key, default)):
            path = Path(tmp) / 'settings.json'
            for dev, saved, override, expected in (
                ('1', 'http://localhost:9999', '', 'http://localhost:9999'),
                ('0', 'http://127.0.0.1:5180', '', 'http://127.0.0.1:5180'),
                ('1', 'http://127.0.0.1:5180', 'http://127.0.0.1:5180', 'http://127.0.0.1:5180'),
            ):
                with self.subTest(dev=dev, saved=saved, override=override), mock.patch.dict(os.environ, FH2MP_DEV=dev, FH2MP_API=override):
                    path.write_text(json.dumps({'api_base': saved}))
                    self.assertEqual(settings.Settings(str(path))['api_base'], expected)
