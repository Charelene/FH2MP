"""A crash report names known causes outside FH2MP for the player (RivaTuner #124/#453, mixed Azimuth files #450)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from fh2mp_launcher import crashreport  # noqa: E402

RTSS_STACK = """CRASH: access violation (0xC0000005) at 0x2BBB45A49F0, thread 13568
  executing address 0x2BBB45A49F0
call stack:
   0  0x2BBB45A49F0
   1  D3D12Core.dll+0x45E07
   2  d3d11.dll+0xB91A7
   4  RTSSHooks64.dll+0x8C83
  10  azimuth.exe+0x4D71A
no mod was on the walked call stack
"""


class CrashHint(unittest.TestCase):
    def crash(self, line="", mod_text=None):
        mod = None
        if mod_text is not None:
            tmp = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
            tmp.write(mod_text)
            tmp.close()
            self.addCleanup(os.unlink, tmp.name)
            mod = tmp.name
        return crashreport.Crash("folder", line, None, 0.0, mod_crash=mod)

    def test_rivatuner_on_the_stack(self):
        hint = crashreport.known_cause(self.crash("CRASHED exception 0xc0000005 at 0x2bbb45a49f0", RTSS_STACK))
        self.assertIn("RivaTuner", hint)
        self.assertIn("azimuth.exe", hint)

    def test_mixed_azimuth_files(self):
        line = "STOPPED SHAPE graph: Azimuth's FFmpeg DLL avcodec-62.dll is not the one this Azimuth ships: its SHA-256 is fe84"
        self.assertIn("setup", crashreport.known_cause(self.crash(line)))

    def test_an_ordinary_crash_has_no_hint(self):
        self.assertEqual(crashreport.known_cause(self.crash("CRASHED exception 0xc0000005 at game+0xf6d928", "call stack:\n   0  game+0xF6D928\n")), "")
        self.assertEqual(crashreport.known_cause(self.crash("")), "")


if __name__ == "__main__":
    unittest.main()
