"""Message Center notifications (db tables `notifications` + `notification_receipts`).

The notification row is immutable content. What one player did with it (read, cleared, claimed a gift) is a receipt
row keyed (notification, xuid), so a broadcast (xuid 0) that one player reads, clears or claims is unchanged for the
others. Only the admin API deletes the notification itself.
"""
import json
import time

SELECT = ("SELECT n.id, n.type, n.message, n.sra, n.sent, n.xuid, n.car_id, n.params, "
          "r.read_at, r.cleared_at, r.claimed_at FROM notifications n "
          "LEFT JOIN notification_receipts r ON r.notification_id = n.id AND r.xuid = ?")


def filetime(unix=None):
    return int(((time.time() if unix is None else unix) + 11644473600) * 10_000_000)


def _n(r):
    n = {"id": r["id"], "type": r["type"], "message": r["message"], "sra": r["sra"], "sent": r["sent"],
         "xuid": r["xuid"], "params": json.loads(r["params"]), "read": r["read_at"] is not None,
         "claimed": r["claimed_at"] is not None}
    if r["car_id"] is not None:
        n["carId"] = r["car_id"]
    return n


class NotificationStore:
    def __init__(self, db):
        self.db = db

    def add(self, message, sra, ntype, params=None, xuid=0, car_id=None, gift_id=False):
        """Queues a notification. `gift_id` fills params["GiftId"] from the new id (gift types need one that the
        client echoes back at retrieval)."""
        with self.db.tx() as c:
            cur = c.execute("INSERT INTO notifications (type, message, sra, sent, xuid, car_id, params) "
                            "VALUES (?,?,?,?,?,?,?)", (ntype, message, sra, filetime(), xuid or 0, car_id,
                                                       json.dumps(params or {})))
            nid = cur.lastrowid
            if gift_id and not (params or {}).get("GiftId"):
                params = dict(params or {}, GiftId=nid & 0x7FFFFFFF)
                c.execute("UPDATE notifications SET params=? WHERE id=?", (json.dumps(params), nid))
            r = c.execute(SELECT + " WHERE n.id=?", (0, nid)).fetchone()
        return _n(r)

    def visible(self, xuid):
        """What a player's Message Center shows: broadcasts plus their own, minus what they cleared."""
        rows = self.db.query(SELECT + " WHERE (n.xuid = 0 OR n.xuid = ?) AND r.cleared_at IS NULL ORDER BY n.id",
                             (xuid, xuid))
        return [_n(r) for r in rows]

    def mark_read(self, xuid, nid):
        self._receipt(xuid, nid, "read_at")

    def clear(self, xuid, nid):
        self._receipt(xuid, nid, "cleared_at")

    def claim(self, xuid, nid):
        """Marks a gift as retrieved by this player. True exactly once per (gift, player), even when two requests
        race: the conditional upsert only touches the row while claimed_at is still NULL."""
        return self._receipt(xuid, nid, "claimed_at") == 1

    def _receipt(self, xuid, nid, column):
        now = time.time()
        with self.db.tx() as c:
            if not c.execute("SELECT 1 FROM notifications WHERE id=? AND (xuid=0 OR xuid=?)", (nid, xuid)).fetchone():
                return 0        # not theirs (or gone): ignore, the client may retry stale ids
            return c.execute(f"INSERT INTO notification_receipts (notification_id, xuid, {column}) VALUES (?,?,?) "
                             f"ON CONFLICT(notification_id, xuid) DO UPDATE SET {column}=excluded.{column} "
                             f"WHERE notification_receipts.{column} IS NULL", (nid, xuid, now)).rowcount

    # ---- admin (not per player)
    def all(self):
        """Every notification with the xuids that read it (`readBy`); `read` means someone did."""
        out = []
        rows = self.db.query("SELECT id, type, message, sra, sent, xuid, car_id, params, NULL AS read_at, "
                             "NULL AS cleared_at, NULL AS claimed_at FROM notifications ORDER BY id")
        for r in rows:
            n = _n(r)
            n["readBy"] = [x[0] for x in self.db.query(
                "SELECT xuid FROM notification_receipts WHERE notification_id=? AND read_at IS NOT NULL", (n["id"],))]
            n["read"] = bool(n["readBy"])
            out.append(n)
        return out

    def get(self, nid):
        return next((n for n in self.all() if n["id"] == nid), None)

    def set_car_id(self, nid, car_id):
        with self.db.tx() as c:
            c.execute("UPDATE notifications SET car_id=? WHERE id=?", (car_id, nid))

    def delete(self, nid):
        with self.db.tx() as c:
            return c.execute("DELETE FROM notifications WHERE id=?", (nid,)).rowcount > 0

    def delete_all(self):
        with self.db.tx() as c:
            return c.execute("DELETE FROM notifications").rowcount

    def count(self):
        return self.db.one("SELECT COUNT(*) FROM notifications")[0]
