"""Smoke tests for the LSP handlers against PostgreSQL: FH2MP_PG_DSN=... python3 -I server/test_lsp.py (needs psycopg 3 and the `game`
catalog schema in the scratch database; the `public` schema is dropped and recreated). Every registered handler is called once with
default parameters (so a broken SQL statement anywhere shows up), then the main flows run end to end."""
import json
import io
import os
import struct
import sys
import unittest
from unittest import mock
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("FH2MP_AUTH", "optional")      # these tests call as legacy clients (X-UserId); the LSP default is required
import fh2mp_lsp as L  # noqa: E402
import test_db  # noqa: E402
import ugc  # noqa: E402
import xls  # noqa: E402

CLS = {"Forza.WebServices.User": "Forza.WebServices", "Xls.WebServices.Content": "Xls.WebServices"}


def clubs_role(name):
    """The wire role number: 1 leader, 2 co-leader, 3 member."""
    return {"leader": 1, "co": 2, "member": 3}[name]


def merge(dst, src):
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            merge(dst[k], v)
        else:
            dst[k] = v


def call(cls, method, params=None):
    """Runs a handler through handle_xls with an encoded request; returns (result code, decoded results or None)."""
    asm = CLS.get(cls, "Forza.WebServices" if cls.startswith("Forza") else "Xls.WebServices")
    pname, pf = L.find_struct(cls, method, "_Parameters")
    body = b""
    if pf is not None:
        p = xls.default_struct(L.SC, pf)
        merge(p, params or {})
        body = xls.encode(L.SC, pf, p)
    out = L.handle_xls(f"{cls}, {asm}", method, body)
    result, error, n = struct.unpack(">III", out[:12])
    rname, rf = L.find_struct(cls, method, "_Results")
    if result == 0 and rf is not None and len(out) > 12 + n:
        return result, xls.decode(L.SC, rf, out[12 + n:])[0]
    return result, None


class StringValidationTests(unittest.TestCase):
    def request(self, body):
        handler = object.__new__(L.Handler)
        handler.path, handler.command = "/system/strings/validate", "POST"
        handler.headers = {"Content-Length": str(len(body)), "X-FH2MP-Host": "client-strings.xboxlive.com"}
        handler.client_address = ("127.0.0.1", 1234)
        handler.rfile, handler.wfile = io.BytesIO(body), io.BytesIO()
        handler.send_response = mock.Mock()
        handler.send_header = mock.Mock()
        handler.end_headers = mock.Mock()
        with mock.patch.object(handler, "_identify", return_value=True), mock.patch.object(L.social, "touch"), \
                mock.patch.object(L.CTX, "xuid", 1, create=True), mock.patch.object(L, "log"):
            handler._handle()
        return handler.send_response.call_args.args[0], json.loads(handler.wfile.getvalue())

    def test_photo_title_and_description_each_get_a_result(self):
        # Actual XSAPI wire spelling, unlike the old handler's stringsToVerify.
        status, response = self.request(json.dumps({"stringstoVerify": ["Sunset", "A drive by the sea"]}).encode())
        self.assertEqual(status, 200)
        self.assertEqual(response, {"verifyStringResult": [
            {"resultCode": 0, "offendingString": ""}, {"resultCode": 0, "offendingString": ""}]})

    def test_legacy_spelling_and_empty_lists_preserve_count(self):
        for key in ("stringstoVerify", "stringsToVerify"):
            for strings in ([], ["one"], ["one", "two", "three"]):
                status, response = self.request(json.dumps({key: strings}).encode())
                self.assertEqual((status, len(response["verifyStringResult"])), (200, len(strings)))

    def test_malformed_input_does_not_fabricate_a_success(self):
        for body in (b"{", b"null", b"{}", b"[]", b'{"stringstoVerify":"title"}', b'{"stringstoVerify":[1]}'):
            status, _ = self.request(body)
            self.assertEqual(status, 400, body)


class LayerCountTests(unittest.TestCase):
    def test_layer_count_always_fits_the_wire_field(self):
        self.assertEqual([L._layer_count(n) for n in (-1, 0, 7, 65535, 70000, None)], [0, 0, 7, 65535, 65535, 0])


    def test_livery_layers_prefers_the_stored_count_and_never_goes_negative(self):
        class NoBlobs:
            def blob_by_name(self, owner, name):
                return None
        old, L.UGC = L.UGC, NoBlobs()
        try:
            self.assertEqual(L._livery_layers({"layers": 18, "creator": 1, "name": "x"}), 18)
            self.assertEqual(L._livery_layers({"layers": -1, "creator": 1, "name": "x"}), 0)        # unknown and no upload left
            self.assertEqual(L._livery_layers({"layers": -1, "creator": 1, "name": ""}), 0)
        finally:
            L.UGC = old


    def test_shim_version_decides_who_may_see_layered_designs(self):
        f = L.shim_has_placement_fix
        self.assertEqual([f(v) for v in ("", "junk", "1.0.0-rc1", "1.0.0-rc2")], [False] * 4)
        self.assertEqual([f(v) for v in ("1.0.0-rc3", "1.0.0-rc10", "1.0.0", "1.0.1", "1.1.0-rc1", "1.0.0-rc2-debug")], [True] * 6)

    def test_min_shim_turns_old_clients_away(self):
        cfg = L.launcher_api.LauncherConfig()
        self.assertEqual(cfg.get()["minShim"], "0.0.0")
        old_launcher, old_auth, old_revoke = L.LAUNCHER, L.AUTH, L.on_revoke
        revoked = []

        class FakeAuth:
            mode = "optional"

            def authenticate(self, token, ip):
                if token != "fh2.good":
                    raise L.auth.AuthError(401, "invalid_token")
                return type("A", (), {"xuid": 0x900000000abc, "gamertag": "Old Timer"})()
        L.LAUNCHER, L.AUTH, L.on_revoke = L.launcher_api.Ctx(None, None, cfg, lambda: 0), FakeAuth(), revoked.append
        L._OLD_REMOVED.clear()
        ctx_before = {k: getattr(L.CTX, k, None) for k in ("xuid", "tag", "authed", "shim")}          # _identify writes the request context
        replies = []

        class Req:
            headers = {}
            _remove_old_client = L.Handler._remove_old_client
            _gamedb_ok = L.Handler._gamedb_ok
            client_ip = lambda self: "127.0.0.1"          # noqa: E731
            _reply_json = lambda self, status, obj, *a, **k: replies.append((status, obj["error"]))     # noqa: E731
        try:
            def ask(shim, token=""):
                replies.clear()
                r = Req()
                r.headers = {**({"X-FH2MP-Shim": shim} if shim else {}), **({"X-FH2MP-Token": token} if token else {})}
                return L.Handler._identify(r), list(replies)
            self.assertEqual(ask(""), (True, []))                          # off by default
            cfg.overlay = {"minShim": "1.0.0-rc4"}
            self.assertEqual(ask(""), (False, [(426, "shim_too_old")]))
            self.assertEqual(ask("1.0.0-rc3"), (False, [(426, "shim_too_old")]))
            self.assertEqual(ask("1.0.0-rc4"), (True, []))
            self.assertEqual(ask("1.0.0"), (True, []))
            self.assertEqual(revoked, [])                                   # nobody was identified so far: nobody removed
            self.assertEqual(ask("1.0.0-rc3", "fh2.bad")[0], False)
            self.assertEqual(revoked, [])                                   # a token that does not check out proves nothing
            self.assertEqual(ask("1.0.0-rc3", "fh2.good")[0], False)
            self.assertEqual(revoked, [0x900000000abc])                     # a verified old client is taken out of the game ...
            ask("1.0.0-rc3", "fh2.good")
            self.assertEqual(len(revoked), 1)                               # ... once a minute, not on every request
            self.assertEqual(ask("1.0.0-rc4", "fh2.good")[0], True)
            self.assertEqual(len(revoked), 1)                               # an up-to-date client is left alone
        finally:
            L.LAUNCHER, L.AUTH, L.on_revoke = old_launcher, old_auth, old_revoke
            for k, v in ctx_before.items():
                setattr(L.CTX, k, v)

    def test_another_car_database_is_turned_away(self):
        import hashlib
        import tempfile
        path = os.path.join(tempfile.mkdtemp(), "gamedbRC.slt")
        with open(path, "wb") as f:
            f.write(b"SQLite format 3\x00" + b"cars" * 100)
        with open(path, "rb") as f:
            good = hashlib.sha256(f.read()).hexdigest()
        old_launcher, old_auth, old_revoke = L.LAUNCHER, L.AUTH, L.on_revoke
        revoked, replies = [], []

        class FakeAuth:
            mode = "optional"

            def authenticate(self, token, ip):
                return type("A", (), {"xuid": 0x900000000def, "gamertag": "Panzer Fan"})()
        L.LAUNCHER = L.launcher_api.Ctx(None, None, L.launcher_api.LauncherConfig(), lambda: 0, gamedb=L.gamedb_sync.GameDb(""))
        L.AUTH, L.on_revoke = FakeAuth(), revoked.append
        L._GAMEDB_REMOVED.clear()
        ctx_before = {k: getattr(L.CTX, k, None) for k in ("xuid", "tag", "authed", "shim")}

        class Req:
            headers = {}
            _gamedb_ok = L.Handler._gamedb_ok
            client_ip = lambda self: "127.0.0.1"          # noqa: E731
            _reply_json = lambda self, status, obj, *a, **k: replies.append((status, obj["error"]))     # noqa: E731
        try:
            def ask(db=None):
                replies.clear()
                r = Req()
                r.headers = {"X-FH2MP-Token": "fh2.x", **({"X-FH2MP-GameDb": db} if db is not None else {})}
                return L.Handler._identify(r), list(replies)
            self.assertEqual(ask("missing"), (True, []))                   # the server sets no database: nothing to compare
            L.LAUNCHER.gamedb = L.gamedb_sync.GameDb(path)
            self.assertEqual(ask(good), (True, []))
            self.assertEqual(ask(good.upper()), (True, []))
            self.assertEqual(ask(None), (True, []))                        # a shim without the check (minShim decides)
            self.assertEqual(revoked, [])
            self.assertEqual(ask("0" * 64), (False, [(409, "gamedb_mismatch")]))
            self.assertEqual(ask("missing"), (False, [(409, "gamedb_mismatch")]))
            self.assertEqual(revoked, [0x900000000def])                    # taken out of the game once a minute, not on every request
        finally:
            L.LAUNCHER, L.AUTH, L.on_revoke = old_launcher, old_auth, old_revoke
            for k, v in ctx_before.items():
                setattr(L.CTX, k, v)

    def test_layered_designs_are_recognised_even_when_the_stored_count_is_wrong(self):
        self.assertFalse(L.design_has_layers(0, b""))
        self.assertTrue(L.design_has_layers(12, b""))
        self.assertTrue(L.design_has_layers(-1, b"xyz"))              # unreadable: hidden, the safe side

    def test_placeholder_thumbnails_have_the_sizes_the_game_writes(self):
        big, small = L._placeholder_thumbs()
        self.assertEqual(L.pngtool.decode(big)[:2], L.BIG_THUMB)
        self.assertEqual(L.pngtool.decode(small)[:2], L.SMALL_THUMB)


class LspTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_db.fresh_db().close()
        L.setup()
        L.CTX.xuid = 0x0009000000000042
        L.TAGS[L.CTX.xuid] = "Tester"

    def setUp(self):
        L.CTX.xuid = 0x0009000000000042

    def test_bots_show_their_names_not_player_numbers(self):
        for nm in L.SB.names[:20]:
            tag = L.SOCIAL.profile(L.SB.fake_xuid(nm))["Gamertag"]
            self.assertEqual(tag, nm[:15])
            self.assertFalse(tag.startswith("Player"), tag)
        self.assertTrue(L.SOCIAL.profile(0x9000000ABCDE)["Gamertag"].startswith("Player"))          # an unknown id still gets a fallback

    def test_every_handler_runs_with_default_parameters(self):
        failures = []
        for (cls, method) in sorted(L.HANDLERS):
            if method == "NotifyRival":          # needs real rival parameters (it queues a notification with typed fields)
                continue
            try:
                call(cls, method)
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cls}.{method}: {type(e).__name__}: {e}")
        self.assertEqual(failures, [])

    def test_logon_creates_the_player(self):
        r, res = call("Forza.WebServices.User", "Logon", {"loginData": {"xuid": L.CTX.xuid, "gamerTag": "Tester",
                                                                       "currentCareerLevel": 7}})
        self.assertEqual(r, 0)
        row = L.DB.one("SELECT gamertag, career_level FROM players WHERE xuid=?", (L.CTX.xuid,))
        self.assertEqual((row["gamertag"], row["career_level"]), ("Tester", 7))

    def test_livery_share_and_download(self):
        container = b"vlrc" + struct.pack("<II", 1, 325) + bytes(range(200))
        comp = zlib.compress(container)
        blob = struct.pack("<II", len(comp), len(container)) + comp
        name = b"Livery_0325_test"
        body = (struct.pack(">I", len(name)) + name + struct.pack(">IH", 325, 1) + b"\x00" + struct.pack(">Q", L.CTX.xuid)
                + struct.pack(">I", len(blob)) + blob)
        up = ugc.parse_livery_upload(body)
        self.assertEqual(up["payload"], blob)
        self.assertEqual(up["container"], container)
        L.profile_update_livery(body)
        vid = L.cv_create_livery({"name": name.decode(), "carId": 325, "LiveryThumbnail": b""})["versionedLiveryId"]
        v = L.UGC.versioned_get(vid)
        self.assertEqual(v["data"], blob)
        self.assertEqual(v["layers"], 1)
        r, res = call("Forza.WebServices.ContentVersioning", "RetrieveVersionedLiveryById", {"versionedLiveryId": vid})
        self.assertEqual(r, 0)
        self.assertEqual(bytes(res["versionedLiveryData"]["data"]), blob)
        fid = L.UGC.file_create("livery", L.CTX.xuid, "Stripe", "", 2, vid, 0, b"\x89PNG", None, 325, 21, 325, 3, 1, 11)
        self.assertEqual([f["id"] for f in L.UGC.search("livery", text="strip")], [fid])        # ILIKE
        self.assertIn(fid, [f["id"] for f in L.UGC.search("livery", gamertag="tester")])           # lower() match on the gamertag
        for method, params, key in (("GetFeaturedLiveriesForRandomCars", {"numCars": 5}, "liveryData"),
                                    ("GetFileHeavyWeightData", {"fileCount": 1, "fileIds": [fid]}, "fileData")):
            r, res = call("Forza.WebServices.Storefront", method, params)
            self.assertEqual([x["Ordinal"] for x in res[key] if x["Id"] == fid], [325], method)    # the car id: "DLC Car" when 0
            previews = [bytes(x["PreviewPayload"]) for x in res[key] if x["Id"] == fid]
            self.assertEqual(previews, [b"\x89PNG"], method)      # served unpacked: a packed blob made every tile blank
        L.UGC.bump(fid, "likes")
        self.assertEqual(L.UGC.file_get(fid)["likes"], 1)
        L.UGC.versioned_ref(vid, -5)
        self.assertEqual(L.UGC.versioned_get(vid)["refs"], 0)                                     # GREATEST(0, ...)

    def test_tune_share_search_and_download(self):
        S = "Forza.WebServices.Storefront"
        r, res = call("Forza.WebServices.ContentVersioning", "CreateVersionedTuneFromStructs", {"carId": 325})
        vid = res["versionedTuneId"]
        r, res = call("Forza.WebServices.StorefrontTuning", "Create", {
            "fileHeader": {"Title": "Grip tune", "Type": 0, "PayloadSize": 10, "KeywordOne": 1, "KeywordTwo": 3},
            "tuningDescription": {"base": {"VersionedTuneId": vid, "MakeId": 21, "ModelId": 325, "ClassId": 3, "PerformanceIndex": 700}},
            "fileName": "tune", "previewData": b"\x89PNG"})
        fid = res["contentFileId"]
        r, res = call(S, "SearchTuningFiles", {"searchParameters": {"ClassId": -1, "KeywordOneId": 0}, "maxFiles": 10})
        found = [x for x in res["searchResults"] if x["Id"] == fid]
        self.assertEqual([x["Ordinal"] for x in found], [325])                      # the car id, taken from the versioned tune
        r, res = call(S, "SearchTuningFiles", {"searchParameters": {"ClassId": 0}, "maxFiles": 10})
        self.assertEqual([x for x in res["searchResults"] if x["Id"] == fid], [])    # wrong class filtered out
        r, res = call(S, "GetFileHeavyWeightData", {"fileCount": 1, "fileIds": [fid]})
        self.assertEqual(res["fileData"][0]["Title"], "Grip tune")
        r, res = call("Forza.WebServices.ContentVersioning", "RetrieveVersionedTuneById", {"versionedTuneId": vid})
        self.assertEqual((r, res["versionedTuneData"]["CarId"]), (0, 325))
        r, res = call(S, "GetFileKeywords", {"fileType": 0, "maxKeywordResults": 50})
        self.assertIn("Drift", [k["Keyword"] for k in res["keywords"]])

    def test_photo_share_upload_list_and_download(self):
        S = "Forza.WebServices.Storefront"
        X = "Xls.WebServices.Content"
        pic = os.urandom(600_000)
        r, res = call("Forza.WebServices.StorefrontPhoto", "Create", {
            "fileHeader": {"Title": "Sunset", "Description": "Nice", "Type": 1, "PayloadSize": len(pic), "KeywordOne": 2},
            "photoDescription": {"base": {"MakeId": 21, "ModelId": 325, "EnvironmentId": 1, "TrackId": 7}},
            "payloadSize": len(pic), "previewData": b"\x89PNGpreview"})
        self.assertEqual(r, 0)
        fid = res["contentFileId"]
        self.assertNotEqual(fid, 0)
        r, res = call(S, "GetPopularPhotos", {"maxFiles": 50})
        self.assertNotIn(fid, [x["Id"] for x in res["PopularPhotos"]])          # hidden until the upload is complete
        for off in range(0, len(pic), 262144):                                  # the client's chunk size, contentId = the file id
            r, res = call(X, "UploadContent", {"contentId": fid, "payloadOffset": off, "payloadTotalSize": len(pic),
                                                "uploadData": pic[off:off + 262144]})
            self.assertEqual((r, res["payloadUploadedSize"]), (0, min(off + 262144, len(pic))))
        L.CTX.xuid = 0x0009000000000043                                          # another player browses and downloads it
        for method, key in (("GetPopularPhotos", "PopularPhotos"), ("GetFeaturedPhotos", "FeaturedPhotos")):
            r, res = call(S, method, {"maxFiles": 50})
            mine = [x for x in res[key] if x["Id"] == fid]
            self.assertEqual([(x["Title"], bytes(x["PreviewPayload"]), x["PayloadSize"]) for x in mine],
                             [("Sunset", b"\x89PNGpreview", len(pic))], method)
        r, res = call(S, "SearchPhotoFiles", {"searchParameters": {"MakeId": 21, "ClassId": -1}, "maxFiles": 10})
        self.assertIn(fid, [x["Id"] for x in res["searchResults"]])
        r, res = call(S, "SearchPhotoFiles", {"searchParameters": {"MakeId": 22, "ClassId": -1}, "maxFiles": 10})
        self.assertNotIn(fid, [x["Id"] for x in res["searchResults"]])
        got, off = b"", 0
        while True:
            r, res = call(X, "DownloadContent", {"contentId": fid, "payloadOffset": off, "requestedSize": 262144})
            got += bytes(res["downloadData"])
            off = len(got)
            if res["isLastBuffer"]:
                break
        self.assertEqual((got, res["totalSize"]), (pic, len(pic)))
        r, _ = call(X, "UploadContent", {"contentId": fid, "payloadOffset": 0, "payloadTotalSize": len(pic), "uploadData": b"x"})
        self.assertNotEqual(r, 0)                                                # somebody else's photo cannot be overwritten

    def test_car_clubs(self):
        C = "Forza.WebServices.Club"
        K = "Forza.WebServices.CarClub"
        A, B, D, E = (0x0009000000001000 + i for i in range(4))
        with L.DB.tx() as c:
            for i, x in enumerate((A, B, D, E)):
                L.db.touch_player(c, x, f"Racer{i}", level=10 + i)
                L.TAGS[x] = f"Racer{i}"
        asx = lambda x: setattr(L.CTX, "xuid", x)
        notes = lambda x: [(n["type"], n["params"]) for n in L.NOTIF.visible(x)]

        asx(A)
        r, res = call(C, "CreateClub", {"clubTag": "AB", "clubName": "x", "privacy": 1})
        self.assertEqual((r, 2), (2, 2))                                        # tag shorter than 3: error result
        r, res = call(C, "CreateClub", {"clubTag": "ZOOM", "clubName": "Zoomers", "clubDescription": "fast", "privacy": 2,
                                        "regionKeywordId": 4, "attitudeKeywordId": 9, "interestKeywordId": 14})
        self.assertEqual(r, 0)
        cid = res["clubId"]
        r, _ = call(C, "CreateClub", {"clubTag": "zoom", "clubName": "Other"})
        self.assertEqual(r, 2)                                                 # tag exists (case-insensitive)
        r, res = call("Xls.WebServices.Club", "GetClubStatus", {"xuid": A})        # the client's class name for the Club* calls
        self.assertEqual(res["clubStatus"]["clubId"], cid)
        r, res = call("Xls.WebServices.User", "GetUserData")                        # the client's own club tag / role after a change
        self.assertEqual((res["userData"]["clubTag"], res["userData"]["roleInClub"], res["userData"]["clubId"]), ("ZOOM", clubs_role("leader"), cid))
        r, res = call(K, "GetClubFullInfo", {"clubId": cid, "maxLeaders": 5})
        d = res["clubDetails"]
        self.assertEqual((d["clubTag"], d["memberCount"], d["clubLeader"]["qwXuid"], d["regionKeyword"]["Keyword"]),
                         ("ZOOM", 1, A, "Europe"))
        self.assertEqual([u["qwXuid"] for u in res["leaders"]], [A])
        r, res = call("Forza.WebServices.Scoreboard", "GetLadderRows", {"clubId": cid, "startAt": 1, "maxResults": 10, "maxTiers": 5})
        self.assertEqual((r, [x["user"]["qwXuid"] for x in res["rows"]], res["userRow"]["rank"], res["available"], len(res["ladderTiers"])),
                         (0, [A], 1, 1, 5))                                      # the club's members, ranked by XP earned in it
        self.assertEqual(res["rows"][0]["user"]["clubTag"], "ZOOM")
        r, res = call("Forza.WebServices.Scoreboard", "GetRows", {"scoreboardType": 8, "scoreType": 4, "view": 0, "isClubBoard": True,
                                                                  "startAt": 1, "maxResults": 20})
        self.assertEqual([(x["rank"], x["clubData"]["clubTag"], x["clubData"]["memberCount"]) for x in res["rows"]], [(1, "ZOOM", 1)])
        self.assertEqual((res["userRow"]["rank"], res["available"]), (1, 1))
        r, res = call(K, "SearchForClubByTag", {"clubTag": "zoom"})
        self.assertEqual(res["clubDetails"]["clubName"], "Zoomers")
        r, res = call(K, "SearchForClubsByName", {"clubName": "zoo", "maxResults": 5})
        self.assertEqual([x["clubTag"] for x in res["clubDetailsList"]], ["ZOOM"])

        asx(B)                                                                 # invite-only: a request, the leader approves
        r, res = call(C, "RequestJoiningClub", {"clubId": cid})
        self.assertEqual((r, res["requestApproved"]), (0, False))
        self.assertIn(0x700, [t for t, _ in notes(A)])
        asx(A)
        r, _ = call(C, "ApproveRequestToJoin", {"approvedUser": B})
        self.assertEqual(r, 0)
        self.assertIn(0x705, [t for t, _ in notes(B)])
        r, res = call(C, "GetClubMembers", {"clubId": cid, "maxMembers": 10})
        self.assertEqual((res["totalMembers"], [m["user"]["qwXuid"] for m in res["clubMembers"]]), (2, [A, B]))
        self.assertEqual(res["clubMembers"][1]["user"]["clubTag"], "ZOOM")

        r, _ = call(C, "InviteToClub", {"userToInvite": D})                      # invitation, accepted from the notification
        self.assertEqual(r, 0)
        inv = [n for n in L.NOTIF.visible(D) if n["type"] == 0x701]
        self.assertEqual(inv[0]["params"]["clubId"], cid)
        r, _ = call(C, "InviteToClub", {"userToInvite": D})
        self.assertEqual(r, 2)                                                 # already invited
        asx(D)
        r, _ = call(C, "AcceptClubInvite", {"clubId": cid, "invitationId": inv[0]["id"]})
        self.assertEqual(r, 0)
        self.assertEqual([n for n in L.NOTIF.visible(D) if n["type"] == 0x701], [])
        asx(B)
        r, _ = call(C, "InviteToClub", {"userToInvite": E})
        self.assertEqual(r, 2)                                                 # a plain member may not invite

        asx(B)                                                                 # Club Garage: B shares a car, A sees and borrows it
        S = "Forza.WebServices.SharedCar"
        r, res = call(S, "UploadCarTextures", {"liveryTexture": b"T" * 300, "LiveryThumbnail": b"P" * 200})
        tid = res["texturesId"]
        guid = bytes(range(16))
        data = {"gameCarId": guid, "ProfileId": bytes(24), "modelId": 325, "perfIndex": 0.74, "classId": 3, "carColorRGB": 255, "isStock": False,
                "speedRating": 6.5, "simPeakPower": 400.0}
        r, res = call(S, "Create", {"completeCarData": data, "texturesId": tid, "liveryDetails": b"LIV" * 10, "tuning": b"TUN" * 5,
                                    "overWriteExisting": False})
        self.assertEqual(r, 0)
        car_id = res["carId"]
        r, res = call(S, "Create", {"completeCarData": data, "texturesId": tid, "liveryDetails": b"NEW", "tuning": b"NEW", "overWriteExisting": True})
        self.assertEqual(res["carId"], car_id)                                 # sharing the same car again overwrites it
        asx(A)
        r, res = call(S, "GetClubCars", {"maxResults": 10})
        c = res["carData"][0]
        self.assertEqual((len(res["carData"]), c["sharedCarId"], c["modelId"], c["makeName"], c["modelName"], c["owner"]["qwXuid"], c["version"]),
                         (1, car_id, 325, "Lamborghini", "Diablo SV", B, 2))
        self.assertAlmostEqual(c["performanceIndex"], 0.74, places=2)
        r, res = call(S, "GetCarDetails", {"carCount": 1, "carIds": [car_id]})
        self.assertEqual((bytes(res["carData"][0]["thumbnail"]), res["carData"][0]["carColorRGB"]), (b"P" * 200, 255))
        r, res = call(S, "DownloadSharedCar", {"carId": car_id, "version": 2, "maxLiveryLength": 1000, "maxTuningLength": 1000})
        self.assertEqual((bytes(res["liveryDetails"]), bytes(res["tuningDetails"])), (b"NEW", b"NEW"))
        r, _ = call(S, "IncrementRaceCount", {"carId": car_id})
        self.assertIn(0x8301, [t for t, _ in notes(B)])                        # the owner hears that someone raced the car
        self.assertEqual(L.CLUBS.club_cars(A, 5)[0]["race_count"], 1)

        asx(A)                                                                 # roles
        r, _ = call(C, "ChangeMemberRole", {"userToChange": B, "newRole": clubs_role("co")})
        self.assertEqual(r, 0)
        self.assertIn(0x708, [t for t, _ in notes(B)])
        r, _ = call(C, "ChangeMemberRole", {"userToChange": B, "newRole": clubs_role("member")})
        self.assertIn(0x704, [t for t, _ in notes(B)])
        r, _ = call(C, "RecommendMemberForRemoval", {"userToRemove": D})
        self.assertEqual(r, 0)
        r, _ = call(C, "RecommendMemberForRemoval", {"userToRemove": D})
        self.assertEqual(r, 2)                                                 # already recommended
        r, res = call(C, "CreateNoticeBoardNotification", {"noticeBoardMessage": "race at 9"})
        self.assertEqual(r, 0)
        self.assertIn(0x709, [t for t, _ in notes(B)])
        r, _ = call(C, "DeleteNoticeBoardNotification", {"notificationId": res["notificationId"]})
        self.assertNotIn(0x709, [t for t, _ in notes(B)])

        r, res = call("Forza.WebServices.SharedCar", "GetClubCars", {"maxResults": 10})
        self.assertEqual(len(res["carData"]), 1)
        r, _ = call(C, "RemoveFromClub", {"userToRemove": D})                    # kick
        self.assertEqual(r, 0)
        self.assertIn(0x703, [t for t, _ in notes(D)])
        asx(B)
        r, _ = call(C, "LeaveClub")                                              # quit (the Leave Club button; RemoveFromClub on oneself does the same)
        self.assertEqual(r, 0)
        self.assertEqual(L.CLUBS.member_info(B), (0, "", 0))
        self.assertEqual(L.CLUBS.club_cars(A, 5), [])                          # a member's shared cars leave with them
        r, res = call(C, "GetClubHistory", {"userXuid": B, "maxEntries": 10})
        self.assertEqual(len(res["userClubHistory"]), 4)                       # joined, promoted, demoted, quit
        asx(A)
        r, _ = call(C, "LeaveClub")                                              # the game's Leave Club button: a leader leaving disbands the club
        self.assertEqual((r, L.CLUBS.club(cid)), (0, None))
        r, res = call(K, "GetRecommendedClubs", {"maxResults": 5})
        self.assertEqual(res["clubDetailsList"], [])

    def test_friends_profiles_and_gamerpics(self):
        A, B, C = (0x0009000000002000 + i for i in range(3))
        with L.DB.tx() as c:
            for i, x in enumerate((A, B, C)):
                L.db.touch_player(c, x, f"Friend{i}", level=5)
        hdr = {"Authorization": f"XBL3.0 x=fh2mp;{A}"}
        self.assertEqual(L.social_api("POST", "/api/friends", json.dumps({"a": hex(A), "b": B}).encode())[0], 201)
        self.assertEqual(L.social_api("POST", "/api/friends", json.dumps({"a": B, "b": A}).encode())[0], 201)        # idempotent, either order
        L.SOCIAL.add_friend(A, C)
        self.assertEqual(L.SOCIAL.friends(A), [B, C])
        self.assertEqual(L.social_api("POST", "/api/friends", json.dumps({"a": A, "b": A}).encode())[0], 400)
        st, obj = L.xbl_social("social.xboxlive.com", "GET", "/users/me/people", b"", hdr)
        self.assertEqual((st, [p["xuid"] for p in obj["people"]], obj["totalCount"]), (200, [str(B), str(C)], 2))
        st, obj = L.xbl_social("profile.xboxlive.com", "POST", "/users/batch/profile/settings",
                               json.dumps({"userIds": [str(B)], "settings": ["Gamertag", "GameDisplayPicRaw", "Gamerscore"]}).encode(), hdr)
        vals = {x["id"]: x["value"] for x in obj["profileUsers"][0]["settings"]}
        self.assertEqual((vals["Gamertag"], vals["Gamerscore"]), ("Friend1", "0"))
        self.assertIn(f"/gamerpic/{B}.png", vals["GameDisplayPicRaw"])
        st, obj = L.xbl_social("profile.xboxlive.com", "GET", "/users/me/profile/settings/people/people?settings=Gamertag,Gamerscore", b"", hdr)
        self.assertEqual([u["id"] for u in obj["profileUsers"]], [str(B), str(C)])
        png = L.SOCIAL.gamerpic(B)                                                  # no upload yet: a generated picture
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        up = L.pngtool.encode(2, 2, 3, [bytearray(b"\xff\x00\x00" * 2)] * 2)
        self.assertEqual(L.social_api("PUT", f"/api/players/{B}/gamerpic", up)[0], 200)
        self.assertEqual(L.social_api("GET", f"/api/players/{B}/gamerpic", b"")[2], up)
        self.assertEqual(L.social_api("PUT", f"/api/players/{B}/gamerpic", b"not a png")[0], 400)
        social_online = L.social.online
        L.social.touch(B)
        st, obj = L.xbl_social("userpresence.xboxlive.com", "POST", "/users/batch", json.dumps({"users": [str(B), str(C)]}).encode(), hdr)
        self.assertEqual([(p["xuid"], p["state"]) for p in obj], [(str(B), "Online"), (str(C), "Offline")])
        self.assertTrue(social_online(B))
        self.assertTrue(L.social_api("DELETE", "/api/friends", json.dumps({"a": A, "b": C}).encode())[1]["removed"])

    def test_scores_and_rivals(self):
        x = L.CTX.xuid
        for score in (95.5, 90.25):
            L.SB.submit(x, "Tester", 1, {"scoreboardType": 3, "gameScoreboardId": 1000, "trackId": 7, "carId": 325, "carClassId": 3,
                                         "carPerformanceIndex": 0.7, "score": L.scoreboard.dbl_to_u64(score), "isClean": True,
                                         "usedSTM": False, "usedTCS": False, "usedABS": True})
        self.assertEqual(L.SB.best_of(x, 3, 1, 7, 1000)["score"], 90.25)
        rows, mine, real, fake = L.SB.rows(x, 3, 1, 7, 1, 20, 1000)
        self.assertEqual(real, 1)
        self.assertEqual(mine["position"] > 0, True)
        self.assertAlmostEqual(L.SB.add_xp(x, 10.0)[0] - L.SB.add_xp(x, 0.0)[0], 0.0)

    def test_leaderboard_career_xp_survives_the_wire_and_restart(self):
        S = "Forza.WebServices.Scoreboard"
        xuid = L.CTX.xuid
        xp = 2410000  # stock curve: level 125, bronze wristband
        code, _ = call("Forza.WebServices.User", "Logon", {
            "loginData": {"xuid": xuid, "gamerTag": "Level125", "currentCareerLevel": xp}})
        self.assertEqual(code, 0)
        self.assertEqual(L.DB.one("SELECT career_level FROM players WHERE xuid=?", (xuid,))[0], xp)
        # The startup path reloads this persisted value into LEVELS.
        L.LEVELS.pop(xuid)
        L.LEVELS[xuid] = L.DB.one("SELECT career_level FROM players WHERE xuid=?", (xuid,))[0]
        for sb, st, pivot in ((9, 5, 27), (1, 1, 2), (3, 1, 1000)):
            d = dict(test_db.sub(50.0, sb=sb), gameScoreboardId=pivot)
            self.assertIsNotNone(L.SB.submit(xuid, "Level125", st, d))
            req = {"scoreboardType": sb, "scoreType": st, "trackId": 7, "pivotId": pivot,
                   "maxResults": 100, "startAt": 1}
            for method in ("GetRows", "GetRowsAnthem"):
                code, res = call(S, method, req)
                self.assertEqual(code, 0)
                mine = next(r for r in res["rows"] if r["user"]["qwXuid"] == xuid)
                self.assertEqual(mine["user"]["currentCareerLevel"], xp)
                self.assertEqual(res["userRow"]["user"]["currentCareerLevel"], xp)
            code, res = call(S, "GetRival", dict(req, rivalXuid=xuid))
            self.assertEqual(code, 0)
            self.assertEqual(res["rival"]["scoreData"]["user"]["currentCareerLevel"], xp)

    def test_stored_events_get_the_class_they_show_enforced(self):
        with L.DB.tx() as c:
            c.execute("DELETE FROM rival_events WHERE id=999001")
            c.execute("INSERT INTO rival_events (id, month, reroll, idx, track, laps, length, kind, attrs, cls, name, trackname, description, created) "
                      "VALUES (999001, 999, 0, 0, 849, 1, 2000.0, 'country', ?, 3, 'Made in Italy Cup', 'Nice West Circuit', '', 0)",
                      (json.dumps({"CountryId": 4}),))
        try:
            self.assertEqual(L.RIV.enforce_stored_classes(), 1)
            row = L.DB.one("SELECT attrs FROM rival_events WHERE id=999001")
            self.assertEqual(json.loads(row["attrs"]), {"CountryId": 4, "CarClassId": 3})
            self.assertEqual(L.RIV.enforce_stored_classes(), 0)            # nothing left to do
        finally:
            with L.DB.tx() as c:
                c.execute("DELETE FROM rival_events WHERE id=999001")

    def test_rival_themes_enforce_the_class_they_show(self):
        # GitHub #2: "Made in Italy" showed an A class card but allowed every Italian car
        used = [t for t in L.RIV.themes() if t["kind"] in L.rivals.CLASS_ENFORCED and not t.get("avoid")]
        self.assertTrue(used)
        for t in used:
            self.assertEqual(t["attrs"].get("CarClassId"), t["cls"], t["title"])
        for e in L.RIV._generate(5, 0):
            if e["kind"] in L.rivals.CLASS_ENFORCED:
                self.assertEqual(e["attrs"].get("CarClassId"), e["cls"], e["name"])

    def test_legacy_jaguar_class_is_enforced_with_only_one_stock_b_car(self):
        with L.DB.tx() as c:
            c.execute("INSERT INTO rival_events (id, month, reroll, idx, track, laps, length, kind, attrs, cls, name, trackname, description, created) "
                      "VALUES (999002, 999, 0, 1, 849, 1, 2000.0, 'make', ?, 2, 'Jaguar Showdown', 'Old Town Circuit', '', 0)",
                      (json.dumps({"MakeId": 19}),))
        try:
            self.assertEqual(L.RIV.enforce_stored_classes(), 1)
            e = L.RIV.by_id(999002)
            self.assertEqual((e["cls"], e["attrs"]), (2, {"MakeId": 19, "CarClassId": 2}))
            self.assertIn('CarClassId="2"', L.game_options_xml(e))
            self.assertEqual(L.RIV.enforce_stored_classes(), 0)
        finally:
            with L.DB.tx() as c:
                c.execute("DELETE FROM rival_events WHERE id=999002")

    def test_rivals_events_are_stored_once(self):
        ev = L.RIV.events()
        self.assertTrue(ev)
        self.assertEqual(L.DB.one("SELECT COUNT(*) FROM rival_events")[0] >= len(ev), True)
        L.RIV._cache = None
        self.assertEqual([e["id"] for e in L.RIV.events()], [e["id"] for e in ev])


if __name__ == "__main__":
    unittest.main()
