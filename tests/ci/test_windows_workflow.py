#!/usr/bin/env python3
"""Check the Windows dispatch matrix and pre-validation recovery artifact."""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


class WindowsWorkflowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(
            (ROOT / '.github/workflows/build.yml').read_text(encoding='utf-8'),
            Loader=yaml.BaseLoader,
        )
        cls.windows = cls.workflow['jobs']['windows']

    def test_platform_dispatch(self):
        options = self.workflow['on']['workflow_dispatch']['inputs']['only']['options']
        self.assertEqual(options, ['all', 'windows', 'windows-x64', 'windows-startup-check', 'linux', 'macos'])
        script = self.workflow['jobs']['detect']['steps'][1]['run']
        for value, windows, linux in [
            ('', 'true', 'true'), ('all', 'true', 'true'),
            ('windows', 'true', 'false'), ('windows-x64', 'true', 'false'),
            ('windows-startup-check', 'false', 'false'),
            ('linux', 'false', 'true'), ('macos', 'false', 'false'),
        ]:
            with self.subTest(only=value), tempfile.NamedTemporaryFile() as output:
                env = dict(os.environ, GITHUB_OUTPUT=output.name)
                subprocess.run(['bash', '-e', '-c', script.replace('${{ inputs.only }}', value)],
                               cwd=ROOT, env=env, check=True, capture_output=True)
                result = dict(line.split('=', 1) for line in Path(output.name).read_text().splitlines())
                self.assertEqual(result, {'has_windows': windows, 'has_linux': linux})

    def test_x64_matrix_and_default_architectures(self):
        expression = self.windows['strategy']['matrix']['include']
        match = re.fullmatch(
            r"\$\{\{\s*fromJSON\(inputs\.only == 'windows-x64' && '([^']+)' \|\| '([^']+)'\)\s*\}\}",
            expression,
        )
        self.assertIsNotNone(match)
        self.assertEqual(json.loads(match[1]), [{'arch': 'x64', 'runner': 'windows-2022'}])
        self.assertEqual(json.loads(match[2]), [
            {'arch': 'x64', 'runner': 'windows-2022'},
            {'arch': 'ARM64', 'runner': 'windows-11-arm'},
        ])

    def test_recovery_precedes_checks_and_release_artifacts(self):
        steps = self.windows['steps']
        names = [step.get('name', '') for step in steps]
        build, stage = names.index('Build'), names.index('Stage')
        recovery = names.index('Upload unverified Windows x64 recovery build')
        self.assertLess(build, stage)
        self.assertLess(stage, recovery)
        self.assertLess(recovery, names.index('Headless self-tests'))
        self.assertLess(recovery, names.index('Installer'))
        self.assertLess(recovery, names.index('Upload'))
        step = steps[recovery]
        self.assertEqual(step['if'], "matrix.arch == 'x64'")
        self.assertEqual(step['continue-on-error'], 'true')
        self.assertEqual(step['with']['name'], 'Infinite-windows-x64-unverified')
        self.assertEqual(step['with']['path'], 'dist/Infinite')
        self.assertEqual(step['with']['if-no-files-found'], 'error')
        self.assertEqual(steps[names.index('Upload')]['with']['name'],
                         'Infinite-windows-${{ matrix.arch }}')


if __name__ == '__main__':
    unittest.main()
