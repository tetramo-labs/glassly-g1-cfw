"""Build and run the host-side C test of the injected handler when a C compiler exists."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'no host C compiler')
class HostHandlerTests(unittest.TestCase):
    def test_injected_handler_on_host(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / 'g1cfw_host_test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
                            '-O1', '-g', str(ROOT / 'tests/host/g1cfw_host_test.c'), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)
