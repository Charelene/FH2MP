"""Smoke tests for the LSP handlers against PostgreSQL: FH2MP_PG_DSN=... python3 -I server/test_lsp.py (needs psycopg 3 and the `game`
catalog schema in the scratch database; the `public` schema is dropped and recreated). Every registered handler is called once with
default parameters (so a broken SQL statement anywhere shows up), then the main flows run end to end."""
import json
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
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
        r, _ = call(C, "RemoveFromClub", {"userToRemove": B})                    # quit
        self.assertEqual(r, 0)
        self.assertEqual(L.CLUBS.member_info(B), (0, "", 0))
        self.assertEqual(L.CLUBS.club_cars(A, 5), [])                          # a member's shared cars leave with them
        r, res = call(C, "GetClubHistory", {"userXuid": B, "maxEntries": 10})
        self.assertEqual(len(res["userClubHistory"]), 4)                       # joined, promoted, demoted, quit
        asx(A)
        r, _ = call(C, "RemoveFromClub", {"userToRemove": A})                    # the leader leaves: the club is disbanded
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
        self.assertAlmostEqual(L.SB.add_xp(x, 10.0) - L.SB.add_xp(x, 0.0), 0.0)

    def test_rivals_events_are_stored_once(self):
        ev = L.RIV.events()
        self.assertTrue(ev)
        self.assertEqual(L.DB.one("SELECT COUNT(*) FROM rival_events")[0] >= len(ev), True)
        L.RIV._cache = None
        self.assertEqual([e["id"] for e in L.RIV.events()], [e["id"] for e in ev])


if __name__ == "__main__":
    unittest.main()
