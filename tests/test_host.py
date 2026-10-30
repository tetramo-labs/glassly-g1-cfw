"""Build and run the host-side C test of the injected handler when a C compiler exists."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined', '-O1', '-g']


def build(exe):
    subprocess.run(CC + [str(ROOT / 'tests/host/g1cfw_host_test.c'), '-o', str(exe)], check=True)


@unittest.skipUnless(shutil.which('cc'), 'no host C compiler')
class HostHandlerTests(unittest.TestCase):
    def test_injected_handler_on_host(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / 'g1cfw_host_test'
            build(exe)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)

    @unittest.skipUnless((ROOT / 'tools/shapes_suite.py').exists(), 'shapes suite not present')
    def test_shapes_suite_dump_replays_on_host(self):
        """Every message the suite's encoder produces is accepted by the firmware parser."""
        with tempfile.TemporaryDirectory() as temp:
            dump = Path(temp) / 'shapes.dump'
            run = subprocess.run([sys.executable, str(ROOT / 'tools/shapes_suite.py'), '--dump', str(dump)],
                                 capture_output=True, text=True, cwd=ROOT)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            exe = Path(temp) / 'g1cfw_host_test'
            build(exe)
            pgm = Path(temp) / 'pgm'
            pgm.mkdir()
            result = subprocess.run([str(exe), '--replay', str(dump), str(pgm)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(' 0 errors', result.stdout)
            self.assertTrue(any(pgm.iterdir()), 'no snapshots written')
