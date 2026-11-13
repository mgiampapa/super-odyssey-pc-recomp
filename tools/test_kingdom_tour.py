"""Comparator safety checks; no game data or running game needed."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import time
import unittest

from PIL import Image, ImageDraw
from kingdom_tour import comparison, stop_test_process


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.reference = self.root / 'reference.png'
        self.candidate = self.root / 'candidate.png'
        self.diff = self.root / 'diff.png'
        Image.new('RGB', (1280, 720)).save(self.reference)

    def test_identical_images_are_within_threshold(self):
        result = comparison(self.reference, self.reference, self.diff)
        self.assertEqual(result['comparison'], 'within_threshold')
        self.assertEqual(result['max_tile_difference'], 0)
        self.assertTrue(self.diff.is_file())

    def test_small_artifact_is_not_hidden_by_screen_average(self):
        image = Image.new('RGB', (1280, 720))
        ImageDraw.Draw(image).rectangle((64, 64, 95, 95), fill='white')
        image.save(self.candidate)
        result = comparison(self.candidate, self.reference, self.diff)
        self.assertLess(result['mean_absolute_difference'], 0.01)
        self.assertEqual(result['comparison'], 'review')
        self.assertEqual(result['max_tile_difference'], 1)

    def test_different_capture_dimensions_require_review(self):
        Image.new('RGB', (640, 360)).save(self.candidate)
        result = comparison(self.candidate, self.reference, self.diff)
        self.assertEqual(result['comparison'], 'size_mismatch')

    @unittest.skipUnless(os.name == 'nt', 'Windows address-layout re-exec cleanup')
    def test_timeout_stops_its_waiting_parent_and_child(self):
        import ctypes
        from ctypes import wintypes
        pid_file = self.root / 'child.pid'
        code = ("import subprocess,sys,time; from pathlib import Path; "
                "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']); "
                "Path(sys.argv[1]).write_text(str(p.pid));p.wait()")
        proc = subprocess.Popen([sys.executable, '-c', code, str(pid_file)],
                                creationflags=subprocess.CREATE_NO_WINDOW)
        child_handle = None
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        try:
            deadline = time.monotonic() + 5
            while not pid_file.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(pid_file.is_file())
            child_handle = kernel.OpenProcess(0x100000, False, int(pid_file.read_text()))
            self.assertTrue(child_handle)
            with (self.root / 'cleanup.log').open('wb') as log:
                stop_test_process(proc, log)
            self.assertIsNotNone(proc.returncode)
            self.assertEqual(kernel.WaitForSingleObject(child_handle, 2000), 0)
        finally:
            if proc.poll() is None:
                with (self.root / 'cleanup.log').open('ab') as log:
                    stop_test_process(proc, log)
            if child_handle:
                kernel.CloseHandle(child_handle)


if __name__ == '__main__':
    unittest.main()
