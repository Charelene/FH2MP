"""Friends UI interactions, offscreen and without accounts, network or game input."""
import os
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from PySide6.QtCore import QEvent, Qt
from PySide6.QtGui import QColor, QImage
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QAbstractButton, QLabel, QScrollArea
from shiboken6 import isValid
from fh2mp_launcher.core import Launcher
from fh2mp_launcher.settings import Settings
from fh2mp_launcher.ui import community, theme
from fh2mp_launcher.ui.main_window import MainWindow
from fh2mp_launcher.ui import images
from fh2mp_launcher.ui.social_widgets import MessageBubble, PersonAvatar
from fh2mp_launcher.ui.widgets import IconButton


class FriendsUi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])
        cls.app.setStyleSheet(theme.QSS)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.env = patch.dict(os.environ, {'FH2MP_LAUNCHER_HOME': self.temp.name, 'FH2MP_NO_KEYRING': '1'})
        self.env.start()
        self.network = patch.object(community, 'run_async', return_value=None)
        self.network.start()
        lc = Launcher(Settings(os.path.join(self.temp.name, 'settings.json')), load_identity=False)
        lc.settings['xuid'] = 9
        lc.settings['gamertag'] = 'TestDriver'
        lc.identity = object()
        self.window = MainWindow(lc)
        self.window.resize(1280, 800)
        self.c = self.window.community
        self.c.action, self.c.refresh, self.c.load_lobbies = Mock(), Mock(), Mock()
        self.person = {'xuid': 1, 'gamertag': 'DriftKing', 'state': 'online', 'club': None, 'lobby': None, 'level': 1}
        self.c.data = {'friends': [self.person], 'incoming': [], 'outgoing': [], 'chats': []}
        self.window.show()
        self.c.changed.emit()
        QTest.qWait(30)

    def tearDown(self):
        self.window.deleteLater()
        self.app.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        self.app.processEvents()
        self.network.stop()
        self.env.stop()
        self.temp.cleanup()

    def test_no_unrequested_navigation_and_panel_uses_animated_width(self):
        texts = [b.text() for b in self.window.findChildren(QAbstractButton)]
        self.assertNotIn('CHALLENGES', texts)
        self.assertNotIn('LEADERBOARDS', texts)
        self.window._toggle_friends()
        QTest.qWait(80)
        self.assertGreater(self.window.friends_panel.width(), 0)
        self.assertLess(self.window.friends_panel.width(), 320)

    def test_minimum_height_keeps_complete_cards_above_carousel_navigation(self):
        w = self.window
        cards = [dict(kind='challenge', tag='MONTHLY CHALLENGE', title='Made in Italy Cup',
                      text='Best: Cprl Punishment · 1:03.042', glyph='A', url='', image='') for _ in range(8)]
        w.cards.set_cards(cards, lambda _: None)
        w.motd.show_message('Working on a fix for 1.0.x Azimuth, should be working properly again very soon today', 'info')
        for opened in (True, False):
            if w.friends_panel.opened != opened:
                w.friends_panel.toggle()
            QTest.qWait(300)
            for width in (980, 1160):
                w.resize(width, 660)
                QTest.qWait(30)
                w.resize(w.width(), w.minimumHeight())
                QTest.qWait(30)
                stage = w.cards.stage
                self.assertGreaterEqual(stage.height(), w.cards.slots[0].height())
                self.assertGreater(w.cards.nav.y(), stage.geometry().bottom())
                self.assertLess(w.cards.geometry().bottom(), w.hero.height())
                self.assertEqual(w.height(), w.minimumHeight())
        QTest.qWait(250)
        self.assertEqual(self.window.friends_panel.width(), 0)
        self.window._toggle_friends()
        QTest.qWait(300)
        self.assertEqual(self.window.friends_panel.width(), 320)

    def test_request_modal_stays_in_window_and_submits_the_entered_player(self):
        dlg = self.c.friends_dialog()
        QTest.qWait(240)
        self.assertFalse(dlg.isWindow())
        self.assertEqual(dlg.shell.width(), 460)
        self.assertLess(dlg.shell.height(), 420)
        QTest.keyClicks(dlg.player, 'DriftKing')
        QTest.mouseClick(dlg.send, Qt.MouseButton.LeftButton)
        self.assertEqual(self.c.action.call_args.kwargs['body'], {'player': 'DriftKing'})
        # Tab stays inside the sheet and Escape animates closed.
        QTest.keyClick(dlg.player, Qt.Key.Key_Tab)
        self.assertTrue(dlg.shell.isAncestorOf(self.app.focusWidget()))
        QTest.keyClick(dlg, Qt.Key.Key_Escape)
        QTest.qWait(250)
        self.assertFalse(isValid(dlg))

    def test_presence_polls_keep_row_identity_and_hover_does_not_grab_focus(self):
        panel = self.window.friends_panel
        row = panel._people[1]
        panel.search.setFocus()
        before = self.app.focusWidget()
        row.show_profile()
        QTest.qWait(30)
        self.assertIs(self.app.focusWidget(), before)
        self.assertFalse(row.popup.isWindow())
        self.c.changed.emit()
        self.assertIs(panel._people[1], row)
        panel.search.setText('nobody')
        self.assertTrue(row.isHidden())
        panel.search.clear()
        self.assertFalse(row.isHidden())

    def test_account_club_tag_survives_general_refresh_and_clears_on_leave(self):
        self.c.data['me'] = {'xuid': 9, 'gamertag': 'TestDriver', 'club': {'tag': 'TEST'}}
        with patch.object(self.window.lc, 'game_running', return_value=True):
            self.c.changed.emit()
            for _ in range(3):
                self.window.render()
                self.assertEqual(self.window.account.state.text(), '[TEST] · In game')
                self.c.changed.emit()
                self.assertEqual(self.window.account.state.text(), '[TEST] · In game')
            self.c.data['me']['club'] = None
            self.c.changed.emit()
            self.assertEqual(self.window.account.state.text(), 'In game')
            self.c.data['me']['club'] = {'tag': 'TEST'}
            self.c.data['me']['xuid'] = 10
            self.window.render()
            self.assertEqual(self.window.account.state.text(), 'In game')
            self.c.data['me']['xuid'] = 9
            self.window.lc.settings['offline_mode'] = True
            self.window.render()
            self.assertEqual(self.window.account.state.text(), 'Offline')

    def test_chat_transition_restores_live_messages_and_preserves_draft_on_resize(self):
        self.c.data['chats'] = [{'id': '1', 'title': 'DriftKing', 'last': 'Hi', 'unread': 0}]
        dlg = self.c.chats()
        QTest.qWait(250)
        dlg.edit.setText('Unsent draft')
        # A message arriving while the entrance is moving must be visible once
        # the live controls replace the transition image.
        dlg.motion.v = 0
        dlg._capture_transition()
        dlg._messages = [{'id': 1, 'sender': 1, 'gamertag': 'DriftKing', 'body': 'Arrived during opening', 'created': 1}]
        dlg.render_messages()
        self.window.resize(1100, 700)
        self.app.processEvents()
        dlg.motion.to(1)
        QTest.qWait(250)
        self.assertTrue(dlg.shell.isVisible())
        self.assertEqual(dlg.edit.text(), 'Unsent draft')
        self.assertIn('Arrived during opening', [b.text for b in dlg.findChildren(MessageBubble) if b.isVisible()])
        self.assertTrue(dlg.rect().contains(dlg.shell.geometry()))
        dlg.edit.setFocus()
        QTest.keyClicks(dlg.edit, '!')
        self.assertEqual(dlg.edit.text(), 'Unsent draft!')
        QTest.keyClick(dlg, Qt.Key.Key_Escape)
        QTest.qWait(250)
        self.assertFalse(isValid(dlg))

    def test_lobby_browser_has_no_empty_scroll_overflow_and_fits_small_window(self):
        row = {'id': 'fbece95a', 'kind': 'freeroam', 'status': 'Free roaming', 'players': 1,
               'occupied': 1, 'capacity': 12, 'held': 0, 'members': []}
        self.c.lobby_data = {'lobbies': [row]}
        self.window.resize(980, 660)
        dlg = self.c.lobbies()
        QTest.qWait(250)
        self.assertTrue(dlg.rect().contains(dlg.shell.geometry()))
        self.assertLess(dlg.shell.height(), 330)
        for area in dlg.table.findChildren(QScrollArea):
            self.assertEqual(area.horizontalScrollBar().maximum(), 0)
            self.assertEqual(area.verticalScrollBar().maximum(), 0)

    def test_home_lobby_card_opens_browser_even_with_zero_active_lobbies(self):
        from fh2mp_launcher.ui.widgets import NewsCard
        self.c.data['lobbyCounts'] = {'freeroam': 0, 'roadtrip': 0}
        self.c.changed.emit()
        QTest.qWait(250)
        card = next(card for card in self.window.cards.findChildren(NewsCard)
                    if card.isVisible() and card.c.get('kind') == 'lobbies')
        QTest.mouseClick(card, Qt.MouseButton.LeftButton)
        QTest.qWait(250)
        browsers = [dlg for dlg in self.window.findChildren(community.LobbiesDialog) if dlg.isVisible()]
        self.assertEqual(len(browsers), 1)
        self.c.load_lobbies.assert_called_once()
        self.assertEqual(card.cursor().shape(), Qt.CursorShape.PointingHandCursor)

    def click_menu_action(self, button, text):
        QTest.mouseClick(button, Qt.MouseButton.LeftButton)
        QTest.qWait(30)
        menu = button.menu
        self.assertTrue(menu.isVisible())
        action = next(a for a in menu.actions() if text in a.text())
        self.assertTrue(action.isEnabled())
        QTest.mouseClick(menu, Qt.MouseButton.LeftButton, pos=menu.actionGeometry(action).center())
        QTest.qWait(30)

    def test_own_id_is_absent_from_add_friend_and_copies_from_account_menu(self):
        dlg = self.c.friends_dialog()
        QTest.qWait(220)
        self.assertNotIn('Your player ID', [l.text() for l in dlg.findChildren(QLabel)])
        dlg.close()
        QTest.qWait(240)
        QTest.mouseClick(self.window.account, Qt.MouseButton.LeftButton)
        QTest.qWait(30)
        action = next(a for a in self.window.account.menu.actions() if a.text() == 'ID 0x9')
        copy = next(b for b in action.defaultWidget().findChildren(IconButton) if b.name == 'copy')
        self.assertEqual(copy.toolTip(), 'Copy account ID')
        QTest.mouseClick(copy, Qt.MouseButton.LeftButton)
        self.window.account.menu.close()
        self.assertEqual(self.app.clipboard().text(), '0x9')

    def test_incoming_requests_are_inline_with_accept_and_decline_actions(self):
        self.c.data['incoming'] = [{**self.person, 'xuid':2}, {**self.person, 'xuid':3}]
        self.c.changed.emit()
        panel = self.window.friends_panel
        self.assertFalse(panel._requests[2].isHidden())
        QTest.mouseClick(panel._requests[2].accept, Qt.MouseButton.LeftButton)
        self.assertEqual(self.c.action.call_args.args, ('/friends/2/accept', 'POST'))
        self.c.action.call_args.kwargs['done']({})
        self.assertNotIn(2, panel._requests)
        QTest.mouseClick(panel._requests[3].reject, Qt.MouseButton.LeftButton)
        self.assertEqual(self.c.action.call_args.args, ('/friends/3/request', 'DELETE'))
        dlg = self.c.friends_dialog()
        self.assertTrue(dlg.request_box.isHidden())

    def test_remove_from_real_dropdown_survives_hover_card_deletion(self):
        row = self.window.friends_panel._people[1]
        row.show_profile()
        card = row.popup
        more = next(b for b in card.findChildren(QAbstractButton) if b.accessibleName() == 'Friend options')
        with patch('PySide6.QtWidgets.QMessageBox.question', side_effect=AssertionError('native confirmation')):
            self.click_menu_action(more, 'Remove friend')
            QTest.qWait(250)
        self.assertFalse(isValid(card))
        confirmation = self.c._dialogs['remove-friend']
        self.assertTrue(isValid(confirmation))
        self.assertFalse(confirmation.isWindow())
        QTest.mouseClick(confirmation.confirm, Qt.MouseButton.LeftButton)
        self.assertEqual(self.c.action.call_args.args, ('/friends/1', 'DELETE'))
        self.c.action.call_args.kwargs['done']({})
        QTest.qWait(230)
        self.assertFalse(isValid(confirmation))
        self.assertEqual(self.c.data['friends'], [])

    def test_unread_indicators_update_without_roster_change_and_chat_menu_works(self):
        self.c.data['chats'] = [{'id':'1', 'title':'DriftKing', 'last':'Hi', 'unread':2}]
        self.c.changed.emit()
        row = self.window.friends_panel._people[1]
        self.assertEqual(row.unread_badge.text(), '2')
        self.assertFalse(row.unread_badge.isHidden())
        self.assertEqual(self.window.friends_button.count, 2)
        dlg = self.c.chats()
        QTest.qWait(220)
        self.assertEqual(dlg.ref, '1')
        more = next(b for b in dlg.findChildren(QAbstractButton) if b.accessibleName() == 'Chat options')
        self.click_menu_action(more, 'Copy player ID')
        self.assertEqual(self.app.clipboard().text(), '0x1')
        self.click_menu_action(more, 'View profile')
        QTest.qWait(220)
        self.assertTrue(isValid(dlg.person_popup))
        self.assertTrue(dlg.person_popup.isVisible())
        self.click_menu_action(more, 'Close chats')
        QTest.qWait(240)
        self.assertFalse(isValid(dlg))

    def test_profile_picture_loads_and_updates_with_full_64_bit_id(self):
        image = QImage(16, 16, QImage.Format.Format_ARGB32)
        image.fill(QColor('#e63946'))
        png = images.png_bytes(image)
        xuid = 0x9000027157ca5
        delivered = []
        self.c.avatar_changed.connect(delivered.append)
        with patch.object(community, 'run_async', side_effect=lambda job, done, failed: done(png)):
            avatar = PersonAvatar({**self.person, 'xuid':xuid}, controller=self.c)
        avatar.show()
        QTest.qWait(20)
        self.assertEqual(delivered, [xuid])
        self.assertEqual(avatar.grab().toImage().pixelColor(18,18).name(), '#e63946')
        self.c.avatar(xuid, 1)
        image.fill(QColor('#2176ae'))
        changed = images.png_bytes(image)
        with patch.object(community, 'run_async', side_effect=lambda job, done, failed: done(changed)):
            self.c.avatar(xuid, 2)
        self.assertEqual(avatar.grab().toImage().pixelColor(18,18).name(), '#2176ae')
        avatar.close()
        avatar.deleteLater()

    def test_message_bubbles_fit_short_text_and_wrap_emoji_and_unbroken_words(self):
        tiny = MessageBubble('Hi')
        self.assertLess(tiny.width(), 60)
        text = '<b>plain text</b>\n' + '👩🏽‍💻' * 100 + 'x' * 300
        bubble = MessageBubble(text, max_width=220)
        self.assertLessEqual(bubble.width(), 220)
        self.assertGreater(bubble.height(), tiny.height())
        self.assertEqual(bubble.document.toPlainText(), text)
        bubble.copy_menu.actions()[0].trigger()
        self.assertEqual(self.app.clipboard().text(), text)
        for w in (tiny, bubble): w.deleteLater()

    def test_scrolled_up_chat_keeps_messages_unread_and_offers_jump_to_latest(self):
        self.c.data['chats'] = [{'id':'1', 'title':'DriftKing', 'last':'Hi', 'unread':2}]
        dlg = self.c.chats()
        self.app.setActiveWindow(self.window)
        dlg._messages = [{'id':i, 'sender':1, 'gamertag':'DriftKing', 'body':f'Message {i}', 'created':0} for i in range(1,51)]
        dlg.last_id = 50
        dlg.render_messages()
        QTest.qWait(240)
        self.assertTrue(self.window.isActiveWindow())
        bar = dlg.messages.verticalScrollBar()
        self.assertGreater(bar.maximum(), 0)
        bar.setValue(0)
        dlg._read_inflight.clear()
        self.c.action.reset_mock()
        dlg._messages.append({'id':51, 'sender':1, 'gamertag':'DriftKing', 'body':'New message', 'created':0})
        dlg.last_id = 51
        dlg.render_messages()
        QTest.qWait(40)
        dlg.mark_visible_read()
        self.c.action.assert_not_called()
        self.assertFalse(dlg.new_messages.isHidden())
        QTest.mouseClick(dlg.new_messages, Qt.MouseButton.LeftButton)
        self.assertEqual(self.c.action.call_args.args, ('/chat/1', 'PATCH', {'lastId':51}))

    def test_chat_covered_by_another_panel_does_not_mark_read(self):
        self.c.data['chats'] = [{'id':'1', 'title':'DriftKing', 'last':'Hi', 'unread':2}]
        dlg = self.c.chats()
        self.app.setActiveWindow(self.window)
        dlg.last_id = 10
        cover = self.c.friends_dialog()
        QTest.qWait(240)
        self.c.action.reset_mock()
        dlg.mark_visible_read()
        self.c.action.assert_not_called()
        self.assertTrue(cover.isVisible())

    def test_panel_errors_stay_inside_the_panel(self):
        dlg = community.SocialDialog(self.c, 'Players', 400, self.window)
        dlg.show()
        dlg.failed(RuntimeError('Could not add friend'))
        QTest.qWait(220)
        self.assertFalse(dlg.error.isWindow())
        self.assertTrue(dlg.shell.isAncestorOf(dlg.error))
        self.assertFalse(dlg.error.isHidden())
        self.assertGreaterEqual(dlg.lay.indexOf(dlg.error), 0)
        dlg.close()


if __name__ == '__main__':
    unittest.main()
