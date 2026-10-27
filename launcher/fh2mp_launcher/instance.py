"""Bring an existing profile window forward through a private local socket."""
import hashlib
import os

from PySide6.QtCore import QObject, Signal
from PySide6.QtNetwork import QLocalServer, QLocalSocket


def server_name(folder):
    path = os.path.normcase(os.path.realpath(folder))
    return 'fh2mp-' + hashlib.sha256(os.fsencode(path)).hexdigest()[:32]


def request_activation(folder):
    socket = QLocalSocket()
    socket.connectToServer(server_name(folder))
    if not socket.waitForConnected(1500):
        return False
    if socket.write(b'activate\n') < 0:
        return False
    socket.flush()
    # Named pipes may complete the write immediately. Waiting for a future
    # bytesWritten signal then times out even though the message was delivered.
    sent = socket.bytesToWrite() == 0 or socket.waitForBytesWritten(1500)
    socket.disconnectFromServer()
    return sent


class InstanceServer(QObject):
    activated = Signal()

    def __init__(self, folder, parent=None):
        super().__init__(parent)
        self._clients = {}
        self.server = QLocalServer(self)
        self.server.setSocketOptions(QLocalServer.SocketOption.UserAccessOption)
        name = server_name(folder)
        # The caller owns the profile lock, so any previous socket is orphaned.
        QLocalServer.removeServer(name)
        if not self.server.listen(name):
            raise OSError('Could not open launcher activation socket: ' + self.server.errorString())
        self.server.newConnection.connect(self._accept)

    def _accept(self):
        while self.server.hasPendingConnections():
            socket = self.server.nextPendingConnection()
            self._clients[socket] = bytearray()
            socket.readyRead.connect(self._read)
            socket.disconnected.connect(self._disconnected)
            if socket.bytesAvailable():
                self._read(socket)

    def _read(self, socket=None):
        socket = socket or self.sender()
        buffer = self._clients.get(socket)
        if buffer is None:
            return
        buffer.extend(bytes(socket.readAll()))
        if len(buffer) > 64:
            socket.abort()
        elif b'\n' in buffer:
            if buffer == b'activate\n':
                self.activated.emit()
            socket.disconnectFromServer()

    def _disconnected(self):
        socket = self.sender()
        self._clients.pop(socket, None)
        socket.deleteLater()

    def close(self):
        for socket in list(self._clients):
            socket.readyRead.disconnect(self._read)
            socket.disconnected.disconnect(self._disconnected)
            socket.abort()
            socket.deleteLater()
        self._clients.clear()
        self.server.close()
