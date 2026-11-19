"""Gamepad control of the launcher: decoding XInput and evdev, repeats, spatial focus moves, and the navigator on a real (offscreen) window."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from fh2mp_launcher import gamepad  # noqa: E402
from fh2mp_launcher.gamepad import IDLE, PadState  # noqa: E402


def pad(*buttons, lx=0.0, ly=0.0, ry=0.0):
    return PadState(frozenset(buttons), lx, ly, 0.0, ry)


class Pure(unittest.TestCase):
    def test_xinput_buttons_and_sticks(self):
        s = gamepad.decode_xinput(0x1000 | 0x0008 | 0x0200 | 0x0010, 32767, -32768, 0, 5000)
        self.assertEqual(s.buttons, frozenset({"a", "right", "rb", "start"}))
        self.assertAlmostEqual(s.lx, 1.0)
        self.assertAlmostEqual(s.ly, -1.0)
        self.assertEqual(s.ry, 0.0)                                       # inside the dead zone
        self.assertEqual(gamepad.decode_xinput(0, 0, 0, 0, 0), IDLE)

    def test_evdev_events(self):
        raw = b"".join(gamepad.EVENT.pack(0, 0, t, c, v) for t, c, v in
                       ((1, 0x130, 1), (3, 0x10, -1), (3, 0x01, 0), (3, 0x00, 255), (1, 0x13b, 1), (1, 0x130, 0)))
        st = gamepad.EvdevState({0x00: (0, 255), 0x01: (0, 255)})
        for ev in gamepad.parse_events(raw + b"\x00" * 5):                # a partial record at the end is left for the next read
            st.apply(*ev)
        s = st.state()
        self.assertEqual(s.buttons, frozenset({"left", "start"}))
        self.assertAlmostEqual(s.lx, 1.0)
        self.assertAlmostEqual(s.ly, 1.0)                                 # evdev y 0 = up

    def test_held_direction(self):
        self.assertEqual(gamepad.held_direction(pad("up")), "up")
        self.assertEqual(gamepad.held_direction(pad(lx=0.9, ly=0.3)), "right")
        self.assertEqual(gamepad.held_direction(pad(lx=0.2, ly=-0.8)), "down")
        self.assertIsNone(gamepad.held_direction(pad(lx=0.4)))

    def test_repeat_timing(self):
        r = gamepad.Repeater()
        moves = []
        for t in range(0, 1001, 16):                                     # right held for one second
            moves += [(t, a) for a in r.feed(pad("right"), t)]
        times = [t for t, _ in moves]
        self.assertEqual(times[0], 0)
        self.assertTrue(400 <= times[1] < 416)
        self.assertTrue(all(70 <= b - a <= 96 for a, b in zip(times[1:], times[2:])))
        self.assertEqual(r.feed(IDLE, 1010), [])
        self.assertEqual(r.feed(pad("a"), 1020), ["a"])
        self.assertEqual(r.feed(pad("a"), 1040), [])                     # held A is one press

    def test_pick_neighbour(self):
        # a 3x2 grid of 100x40 cells, 20 px apart, plus a wide bar below
        cells = {(c, r): (c * 120, r * 60, 100, 40) for c in range(3) for r in range(2)}
        cells["bar"] = (0, 130, 340, 40)
        pool = list(cells.items())
        cur = cells[(1, 0)]
        others = [(k, v) for k, v in pool if v != cur]
        self.assertEqual(gamepad.pick_neighbour(cur, others, "right"), (2, 0))
        self.assertEqual(gamepad.pick_neighbour(cur, others, "left"), (0, 0))
        self.assertEqual(gamepad.pick_neighbour(cur, others, "down"), (1, 1))
        self.assertIsNone(gamepad.pick_neighbour(cur, others, "up"))
        cur = cells[(2, 1)]
        self.assertEqual(gamepad.pick_neighbour(cur, [(k, v) for k, v in pool if v != cur], "down"), "bar")


class FakePad:
    def __init__(self):
        self.state = IDLE

    def poll(self):
        return self.state


class NavigatorOnAWindow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from PySide6.QtWidgets import QGridLayout, QLineEdit, QPushButton, QWidget
        self.win = QWidget()
        self.win.resize(600, 400)
        grid = QGridLayout(self.win)
        self.buttons = {}
        for r in range(2):
            for c in range(3):
                b = QPushButton(f"{c},{r}")
                grid.addWidget(b, r, c)
                self.buttons[(c, r)] = b
        self.field = QLineEdit()
        grid.addWidget(self.field, 2, 0, 1, 3)
        self.win.show()
        self.win.activateWindow()
        self.app.processEvents()
        self.clicked = []
        for k, b in self.buttons.items():
            b.clicked.connect(lambda _=False, k=k: self.clicked.append(k))
        self.backend = FakePad()
        self.running = False
        self.nav = gamepad.Navigator(self.app, self.backend, suppressed=lambda: self.running, home=self.buttons[(0, 0)])
        self.nav.scope = lambda: self.win
        self.t = 0

    def tearDown(self):
        self.nav.timer.stop()
        self.app.removeEventFilter(self.nav)
        self.win.close()
        self.win.deleteLater()
        self.app.processEvents()

    def press(self, *buttons):
        self.t += 100
        self.nav.feed(pad(*buttons), self.t)
        self.t += 100
        self.nav.feed(IDLE, self.t)
        self.app.processEvents()

    def focused(self):
        from PySide6.QtWidgets import QApplication
        w = QApplication.focusWidget()
        return next((k for k, b in self.buttons.items() if b is w), w)

    def test_first_press_focuses_home_then_moves(self):
        self.press("right")
        self.assertEqual(self.focused(), (0, 0))
        self.assertTrue(self.nav.pad_mode)
        self.press("right")
        self.assertEqual(self.focused(), (1, 0))
        self.press("down")
        self.assertEqual(self.focused(), (1, 1))
        self.press("down")
        self.assertIs(self.focused(), self.field)
        self.press("up")
        self.assertEqual(self.focused(), (1, 1))

    def test_a_clicks(self):
        self.buttons[(2, 1)].setFocus()
        self.press("a")
        self.app.processEvents()
        from PySide6.QtTest import QTest
        QTest.qWait(250)                                                 # animateClick releases after 100 ms
        self.assertEqual(self.clicked, [(2, 1)])

    def test_a_on_a_text_field_opens_the_keyboard(self):
        self.field.setFocus()
        self.press("a")
        kb = [w for w in self.win.findChildren(gamepad.PadKeyboard) if w.isVisible()]
        self.assertEqual(len(kb), 1)
        kb[0].type("h")
        kb[0].type("i")
        kb[0].pad_action("y")
        kb[0].type("x")
        kb[0].pad_action("x")
        self.assertEqual(self.field.text(), "hi ")
        kb[0].pad_action("start")
        self.app.processEvents()
        self.assertFalse(kb[0].isVisible())
        self.assertIs(self.focused(), self.field)

    def test_nothing_while_the_game_runs(self):
        self.running = True
        self.backend.state = pad("a")
        self.nav.tick()
        self.backend.state = IDLE
        self.nav.tick()
        self.assertFalse(self.nav.pad_mode)

    def test_start_and_y_signal(self):
        got = []
        self.nav.play.connect(lambda: got.append("play"))
        self.nav.friends.connect(lambda: got.append("friends"))
        self.nav.shoulder.connect(lambda s: got.append(s))
        self.buttons[(0, 0)].setFocus()
        self.press("start")
        self.press("y")
        self.press("rb")
        self.press("lb")
        self.assertEqual(got, ["play", "friends", 1, -1])

    def test_b_closes_a_dialog(self):
        from PySide6.QtWidgets import QDialog, QPushButton, QVBoxLayout
        dlg = QDialog(self.win)
        lay = QVBoxLayout(dlg)
        ok = QPushButton("OK")
        lay.addWidget(ok)
        dlg.show()
        self.app.processEvents()
        ok.setFocus()
        self.nav.scope = lambda: dlg
        self.press("b")
        self.assertFalse(dlg.isVisible())
        self.assertEqual(dlg.result(), QDialog.DialogCode.Rejected)

    def test_mouse_ends_pad_mode(self):
        from PySide6.QtCore import QPointF, Qt
        from PySide6.QtGui import QMouseEvent
        from PySide6.QtCore import QEvent
        self.press("right")
        self.assertTrue(self.nav.ring.isVisible())
        ev = QMouseEvent(QEvent.Type.MouseMove, QPointF(5, 5), QPointF(5, 5), Qt.MouseButton.NoButton, Qt.MouseButton.NoButton,
                         Qt.KeyboardModifier.NoModifier)
        self.app.sendEvent(self.win, ev)
        self.assertFalse(self.nav.pad_mode)
        self.assertFalse(self.nav.ring.isVisible())


if __name__ == "__main__":
    unittest.main()
