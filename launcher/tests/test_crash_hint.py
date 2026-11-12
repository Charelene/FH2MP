"""A crash report names known causes outside FH2MP for the player (RivaTuner #124/#453, mixed Azimuth files #450, fh2wheels.dll #457,
a garage car with no engine data (12e0de0), a graphics device reset #458/#462, a full disk #463/#464)."""
import os
import shutil
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

# shaped like report 2cb2ec5004c023d3 (2026-11-11; frames 12E0DF5, 12E1146, 12D2047, 1044E4F and 2D04AF are from the catalog):
# the save load rates a garage car whose torque curve has no samples
GARAGE_STACK = """CRASH: access violation (0xC0000005) at game+0x12E0DF5, thread 9100
  writing address 0x288FEDF2000
call stack:
   0  game+0x12E0DF5
   1  game+0x12E1146
   2  game+0x12D2047
   3  game+0x12CF8A1
   4  game+0x1044E4F
   5  game+0x2D04AF
   6  game+0x2CFAD2
   7  game+0x2BE9F0
no mod was on the walked call stack
"""
# the same constructor reached from a tune listing, not from the save load: not this cause
TUNE_STACK = """CRASH: access violation (0xC0000005) at game+0x12E0DE0, thread 4410
call stack:
   0  game+0x12E0DE0
   1  game+0x12E1146
   2  game+0x1044E4F
   3  game+0xE9AB3B
FH2MP hooks:
  * FH2MP: function game+0x2D0300 (example)
"""
WHEELS_STACK = """CRASH: access violation (0xC0000005) at fh2wheels.dll+0x20D01, thread 7484
  writing address 0x1402F6DF2
call stack:
   0  fh2wheels.dll+0x20D01
   1  fh2wheels.dll+0x47E6
   2  KERNEL32.DLL+0x17374
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

    def test_wheel_mod_dll(self):
        self.assertIn("fh2wheels.dll", crashreport.known_cause(self.crash("CRASHED exception 0xc0000005 at 0x59120d01", WHEELS_STACK)))

    def test_garage_car_without_engine_data(self):
        hint = crashreport.known_cause(self.crash("CRASHED exception 0xc0000005 at game+0x12e0df5", GARAGE_STACK))
        self.assertIn("car pack", hint)

    def test_torque_curve_crash_elsewhere_is_not_the_garage(self):
        # the save-load frame must be on the call stack itself, not in the hook list
        self.assertEqual(crashreport.known_cause(self.crash("CRASHED exception 0xc0000005 at game+0x12e0de0", TUNE_STACK)), "")

    def test_graphics_device_removed(self):
        line = "STOPPED D3D12 device removed HRESULT=0x887a0006; DRED takes it for a page fault; no page fault recorded"
        self.assertIn("graphics driver", crashreport.known_cause(self.crash(line)))

    def azimuth_log(self, text):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        os.makedirs(os.path.join(folder, "data", "logs"))
        with open(os.path.join(folder, "data", "logs", "azimuth.log"), "w", encoding="utf-8") as f:
            f.write(text)
        return folder

    def test_full_disk(self):
        folder = self.azimuth_log(
            "10 tid=1 session 9f24 began 2026-11-12 13:20:00 UTC: Azimuth 1.0.4\n"
            "2797843 tid=11356 local save update failed: Local save quota or disk space exhausted\n"
            "3167265 tid=10108 graphics pipeline library not saved: error 112; each save tries again\n"
            "3301000 tid=3920 CRASHED exception 0xc0000005 at 0x7ffe3986c299 reading 0x2191048c000 on thread 3920\n")
        crash = crashreport.Crash(folder, "CRASHED exception 0xc0000005 at 0x7ffe3986c299", None, 0.0)
        self.assertIn("Free some space", crashreport.known_cause(crash))

    def test_full_disk_of_an_earlier_session_does_not_count(self):
        folder = self.azimuth_log(
            "10 tid=1 session 1111 began 2026-11-11 10:00:00 UTC: Azimuth 1.0.4\n"
            "3167265 tid=10108 graphics pipeline library not saved: error 112; each save tries again\n"
            "10 tid=1 session 2222 began 2026-11-12 13:20:00 UTC: Azimuth 1.0.4\n"
            "3301000 tid=3920 CRASHED exception 0xc0000005 at game+0xf6d928 reading 0x0 on thread 3920\n")
        crash = crashreport.Crash(folder, "CRASHED exception 0xc0000005 at game+0xf6d928", None, 0.0)
        self.assertEqual(crashreport.known_cause(crash), "")

    def test_an_ordinary_crash_has_no_hint(self):
        self.assertEqual(crashreport.known_cause(self.crash("CRASHED exception 0xc0000005 at game+0xf6d928", "call stack:\n   0  game+0xF6D928\n")), "")
        self.assertEqual(crashreport.known_cause(self.crash("")), "")


if __name__ == "__main__":
    unittest.main()
