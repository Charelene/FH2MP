package lsp

import (
	"fmt"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"

	"fh2mp/internal/diag"
	"fh2mp/internal/parts"
	"fh2mp/internal/pngtool"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/ugc"
	"fh2mp/internal/xls"
)

// User content: paint jobs, tunes, vinyl groups, photos (package ugc).

func init() {
	const cv = "Forza.WebServices.ContentVersioning"
	const st = "Forza.WebServices.Storefront"
	register("Xls.WebServices.Content", "UploadContent", contentUpload)
	register("Xls.WebServices.Content", "DownloadContent", contentDownload)
	register(cv, "CreateVersionedLivery", cvCreateLivery)
	register(cv, "CreateVersionedLayerGroup", cvCreateLayerGroup)
	register(cv, "CreateVersionedTuneFromStructs", cvCreateTuneStructs)
	register(cv, "RetrieveVersionedLiveryById", cvGetLivery)
	register(cv, "RetrieveVersionedLayerGroupById", cvGetLayerGroup)
	register(cv, "RetrieveVersionedTuneById", cvGetTune)
	register(cv, "RemoveReferenceToVersionedLivery", cvRef("versionedLiveryId", -1))
	register("Forza.WebServices.StorefrontLivery", "Create", storeLiveryCreate)
	register("Forza.WebServices.StorefrontTuning", "Create", storeTuningCreate)
	register("Forza.WebServices.StorefrontLayerGroup", "Create", storeLayerGroupCreate)
	register(st, "SearchLayerGroupFiles", func(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
		return c.searchFiles(r, "layergroup", "ForzaStorefrontSearchLayerGroupFiles_Results", "SearchLayerGroupFiles", req)
	})
	register(st, "SearchLiveryFiles", func(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
		return c.searchFiles(r, "livery", "ForzaStorefrontSearchLiveryFiles_Results", "SearchLiveryFiles", req)
	})
	register(st, "SearchTuningFiles", func(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
		return c.searchFiles(r, "tune", "ForzaStorefrontSearchTuningFiles_Results", "SearchTuningFiles", req)
	})
	register("Forza.WebServices.StorefrontPhoto", "Create", storePhotoCreate)
	register(st, "SearchPhotoFiles", func(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
		return c.searchFiles(r, "photo", "ForzaStorefrontSearchPhotoFiles_Results", "SearchPhotoFiles", req)
	})
	register(st, "GetPopularPhotos", storePopularPhotos)
	register(st, "GetFeaturedPhotos", storeFeaturedPhotos)
	register(st, "GetFileHeavyWeightData", storeHeavy)
	register(st, "GetMyFileList", storeMyFiles)
	register(st, "GetFileDataIfShared", storeIfShared)
	register(st, "IncrementLikes", bumpHandler("likes"))
	register(st, "IncrementUsage", bumpHandler("uses"))
	register(st, "IncrementViews", bumpHandler("views"))
	register(st, "RemoveFile", storeRemove)
	register(st, "GetFileKeywords", ugcKeywords)
	register(st, "GetFeaturedLiveriesForRandomCars", storeFeaturedLiveries)
	register(st, "GetLiveriesForDrivatar", storeDrivatarLiveries)
	register(st, "GetCuratedContent", storeCurated)
	register(st, "ReportContent", noop)
	register(st, "ToggleFollowedCreator", noop)
	rawHandlers[[2]string{"Forza.WebServices.OnlineProfile", "UpdateLivery"}] = profileUpdateLivery
	rawHandlers[[2]string{"Forza.WebServices.OnlineProfile", "UpdateLiveryGroup"}] = profileUpdateLayerGroup
}

func noop(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) { return nil, nil }

// Keyword lists as Forza Horizon 4 has them: per file type two pickers (bucket 0 and bucket 1), a file carries one keyword of each. The client's file
// types: 0 tune, 1 vinyl group, 2 design (livery), 3 replay, 4 photo. A keyword's id is its position in the two lists together, starting at 1.
var keywordLists = map[int64][2][]string{
	0: {{"Top Speed", "Off the Line", "Grip", "Drift", "Drag", "Circuit", "All Around"},
		{"Best for Speed", "Good for Speed", "Average", "Best for Handling", "Good for Handling", "Beginner", "Novice", "Expert", "Good All Around", "Close-Ratio Gearbox", "Wide-Ratio Gearbox"}},
	1: {{"Logo", "Racing", "Replica", "Real World", "Cartoon", "Funny", "Abstract", "Fantasy", "Sci-Fi", "Anime"},
		{"Realistic", "Manufacturer", "Original", "Team", "Character", "Scary", "Humorous", "Big", "Small", "Narrow", "Wide"}},
	2: {{"Racing", "Replica", "Real World", "Cartoon", "Funny", "Abstract", "Fantasy", "Sci-Fi", "Photo"},
		{"Realistic", "Original", "Team", "Classic", "Throwback", "Scene", "Character", "Scary", "Humorous", "Drift"}},
	3: {{"Racing", "Rally", "Drift", "Retro", "Classic", "Anime", "Sponsored", "Clean", "Camo", "Flames"},
		{"Stripes", "Cartoon", "Movie", "Game", "Sports", "Original", "Tribute", "Police", "Tuner", "Art"}},
	4: {{"Racing", "Scenery", "Group", "Action", "Humorous"}, {"Filtered", "B&W", "Stylized", "Untouched"}},
}

var fileKind sync.Map // the client's file type number -> kind, learned from Create calls

func ugcKeywords(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	lists, ok := keywordLists[req.Int("fileType")]
	if !ok { // a file type the client has not been seen to ask for
		lists = [2][]string{keywordLists[3][0], nil}
		lists[0] = append(append([]string(nil), keywordLists[3][0]...), keywordLists[3][1]...)
	}
	first, second := lists[0], lists[1]
	all := append(append([]string(nil), first...), second...)
	var rows []any
	for i, k := range all {
		row := xls.NewStruct()
		row.Set("Keyword", k)
		row.Set("KeywordId", int64(i+1))
		bucket := int64(1)
		if i < len(first) {
			bucket = 0
		}
		row.Set("Bucket", bucket)
		rows = append(rows, row)
	}
	n := int(max(0, req.Int("maxKeywordResults")))
	if len(rows) > n {
		rows = rows[:n]
	}
	if rows == nil {
		rows = []any{}
	}
	out := xls.NewStruct()
	out.Set("keywords", rows)
	return out, nil
}

// ordinal of a storefront item is the car id: the client's MakeName / ModelName / Year bindings read it and show "DLC Car" when it is 0 or unknown.
func ordinal(f *ugc.File) int64 {
	if f.CarID == nil {
		return 0
	}
	return *f.CarID & 0xFFFF
}

func (c *Core) light(f *ugc.File, tmpl *xls.Struct) *xls.Struct {
	r := tmpl.Clone()
	r.Set("Id", f.ID)
	r.Set("PopularityRating", int64(f.Stars()))
	r.Set("Creator", c.owner(tmpl.Sub("Creator"), f.Owner))
	r.Set("IsCreatorFollowed", false)
	r.Set("SubmissionTime", iso(f.Created))
	r.Set("Ordinal", ordinal(f))
	r.Set("MethodObtained", int64(0))
	return r
}

func (c *Core) heavy(f *ugc.File, tmpl *xls.Struct) *xls.Struct {
	r := tmpl.Clone()
	r.Set("Id", f.ID)
	r.Set("Type", f.Type)
	r.Set("Title", f.Title)
	r.Set("Description", f.Description)
	r.Set("SubmissionTime", iso(f.Created))
	r.Set("Owner", c.owner(tmpl.Sub("Owner"), f.Owner))
	r.Set("PayloadSize", f.PayloadSize)
	pv := f.Preview
	if len(pv) > 409600 {
		pv = pv[:409600]
	}
	r.Set("PreviewPayload", append([]byte{}, pv...))
	r.Set("Ordinal", ordinal(f))
	r.Set("IsUnderReview", false)
	var vid int64
	if f.VersionedID != nil {
		vid = *f.VersionedID
	}
	r.Set("VersionedContentId", vid)
	r.Set("PopularityRating", int64(f.Stars()))
	r.Set("IsCreatorFollowed", false)
	return r
}

func limitErr(err error) error {
	if le, ok := err.(*ugc.LimitError); ok {
		return &XlsError{le.Msg, 1}
	}
	return err
}

func contentUpload(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	data := req.Bytes("uploadData")
	contentID := req.Int("contentId")
	blob, err := c.Ugc.ContentBlob(contentID)
	if err != nil {
		return nil, err
	}
	photo, err := c.Ugc.IsPhoto(contentID)
	if err != nil {
		return nil, err
	}
	var finish func([]byte) []byte
	if photo {
		finish = ugc.Photo1080p
	}
	got, ok, err := c.Ugc.BlobWrite(blob, req.Int("payloadOffset"), data, r.Xuid, finish)
	if err != nil {
		return nil, limitErr(err)
	}
	have := "unknown id (or not yours)"
	if ok {
		have = fmt.Sprintf("have %d", got)
	}
	c.Log(r, fmt.Sprintf("  Content.UploadContent id=%d @%d +%d of %d: %s", contentID, req.Int("payloadOffset"), len(data), req.Int("payloadTotalSize"), have))
	if !ok {
		return nil, xerr("no such content %d", contentID)
	}
	out := xls.NewStruct()
	out.Set("payloadUploadedSize", got)
	return out, nil
}

func contentDownload(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	contentID := req.Int("contentId")
	blob, err := c.Ugc.ContentBlob(contentID)
	if err != nil {
		return nil, err
	}
	ch, err := c.Ugc.BlobRead(blob, req.Int("payloadOffset"), req.Int("requestedSize"))
	if err != nil {
		return nil, limitErr(err)
	}
	if ch == nil {
		return nil, xerr("no such content %d", contentID)
	}
	c.Log(r, fmt.Sprintf("  Content.DownloadContent id=%d @%d +%d/%d last=%s", contentID, req.Int("payloadOffset"), len(ch.Data), ch.Total, b2s(ch.Last)))
	out := xls.NewStruct()
	out.Set("isLastBuffer", ch.Last)
	out.Set("downloadData", ch.Data)
	out.Set("totalSize", int64(ch.Total))
	return out, nil
}

// profileUpdateLivery is OnlineProfile.UpdateLivery: the real upload of a design (layout in ugc.ParseLiveryUpload). It is kept under (owner, name) until
// CreateVersionedLivery names it.
func profileUpdateLivery(c *Core, r *Req, body []byte) []byte {
	up := ugc.ParseLiveryUpload(body)
	if up == nil {
		c.Warn(r, fmt.Sprintf("  UpdateLivery: unparsable body (%dB), user=%#x; nothing kept, the share that follows will find no upload", len(body), r.Xuid))
		return nil
	}
	bid, err := c.Ugc.BlobCreate(r.Xuid, up.Name, int64(len(body)))
	if err == nil {
		_, _, err = c.Ugc.BlobWrite(bid, 0, body, r.Xuid, nil)
	}
	if err != nil {
		if le, ok := err.(*ugc.LimitError); ok {
			c.Warn(r, fmt.Sprintf("  UpdateLivery refused: %s (user=%#x, %dB)", le.Msg, r.Xuid, len(body)))
			return nil
		}
		panic(err)
	}
	c.Log(r, fmt.Sprintf("  UpdateLivery %s car=%d layers=%d container=%dB (%dB on the wire) -> kept as blob #%d", pyjson.PyRepr(up.Name), up.CarID, up.Layers, len(up.Container), len(body), bid))
	return nil
}

// profileUpdateLayerGroup is OnlineProfile.UpdateLiveryGroup: the upload of a vinyl group, the same request as UpdateLivery without the car. It is
// kept under (owner, name) until CreateVersionedLayerGroup names it.
func profileUpdateLayerGroup(c *Core, r *Req, body []byte) []byte {
	up := ugc.ParseLayergroupUpload(body)
	if up == nil {
		c.Warn(r, fmt.Sprintf("  UpdateLiveryGroup: unparsable body (%dB), user=%#x; nothing kept, the share that follows will find no upload", len(body), r.Xuid))
		return nil
	}
	bid, err := c.Ugc.BlobCreate(r.Xuid, up.Name, int64(len(body)))
	if err == nil {
		_, _, err = c.Ugc.BlobWrite(bid, 0, body, r.Xuid, nil)
	}
	if err != nil {
		if le, ok := err.(*ugc.LimitError); ok {
			c.Warn(r, fmt.Sprintf("  UpdateLiveryGroup refused: %s (user=%#x, %dB)", le.Msg, r.Xuid, len(body)))
			return nil
		}
		panic(err)
	}
	c.Log(r, fmt.Sprintf("  UpdateLiveryGroup %s layers=%d payload=%dB -> kept as blob #%d", pyjson.PyRepr(up.Name), up.Layers, len(up.Payload), bid))
	return nil
}

var bigThumb, smallThumb = [2]int{816, 459}, [2]int{192, 108} // design thumbnails as the game writes them (RGBA PNG)

var (
	placeholderOnce  sync.Once
	placeholderBig   []byte
	placeholderSmall []byte
)

// placeholderThumbs are plain mid-grey RGBA PNGs at the sizes the game writes, built once.
func placeholderThumbs() ([]byte, []byte) {
	placeholderOnce.Do(func() {
		mk := func(w, h int) []byte {
			row := make([]byte, 0, w*4)
			for i := 0; i < w; i++ {
				row = append(row, 128, 128, 128, 255)
			}
			rows := make([][]byte, h)
			for i := range rows {
				rows[i] = row
			}
			return pngtool.Encode(w, h, 4, rows)
		}
		placeholderBig, placeholderSmall = mk(bigThumb[0], bigThumb[1]), mk(smallThumb[0], smallThumb[1])
	})
	return placeholderBig, placeholderSmall
}

// liveryThumbs makes (big, small) design thumbnails from the Storefront preview (an 816x459 PNG): the client saves both with a downloaded design and
// crashes in LiveryAsyncSaveTask without them / with the wrong size.
func (c *Core) liveryThumbs(r *Req, preview []byte) (big, small []byte) {
	if len(preview) < 4 || string(preview[:4]) != "\x89PNG" {
		return nil, nil
	}
	im, err := pngtool.Decode(preview, pngtool.MaxDim) // dimensions are enforced inside the decoder
	if err == nil {
		if [2]int{im.W, im.H} == bigThumb {
			big = preview
		} else {
			big, err = pngtool.Downscale(preview, bigThumb[0], bigThumb[1])
		}
	}
	if err == nil {
		small, err = pngtool.Downscale(preview, smallThumb[0], smallThumb[1])
	}
	if err != nil {
		c.Warn(r, fmt.Sprintf("  thumbnail generation failed, no thumbnail stored: %v", err)) // never fail a share over a thumbnail
		return nil, nil
	}
	return big, small
}

func cvCreateLivery(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	name, carID := req.Str("name"), req.Int("carId")
	thumb := req.Bytes("LiveryThumbnail")
	vid, err := c.Ugc.VersionedCreate("livery", r.Xuid, name, &carID, thumb, 0, nil, nil, nil)
	if err != nil {
		return nil, err
	}
	bid, bdata, err := c.Ugc.BlobByName(r.Xuid, name)
	if err != nil {
		return nil, err
	}
	note := "no matching UpdateLivery upload"
	if bid != 0 {
		if up := ugc.ParseLiveryUpload(bdata); up != nil && len(up.Container) > 0 {
			if _, err := c.Ugc.VersionedSetData(vid, up.Payload, 0); err != nil {
				return nil, err
			}
			if _, err := c.DB.Exec("UPDATE ugc_versioned SET layers=?, car_id=COALESCE(NULLIF(car_id, 0), ?) WHERE id=?", up.Layers, up.CarID, vid); err != nil {
				return nil, err
			}
			note = fmt.Sprintf("data %dB (inflates to %dB), %d layers", len(up.Payload), len(up.Container), up.Layers)
		}
	}
	c.Log(r, fmt.Sprintf("  CreateVersionedLivery %s car=%d thumb=%dB -> versioned %d (%s)", pyjson.PyRepr(name), carID, len(thumb), vid, note))
	out := xls.NewStruct()
	out.Set("versionedLiveryId", vid)
	return out, nil
}

func cvCreateLayerGroup(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	name := req.Str("name")
	thumb := req.Bytes("thumbnail")
	vid, err := c.Ugc.VersionedCreate("layergroup", r.Xuid, name, nil, thumb, 0, nil, nil, nil)
	if err != nil {
		return nil, err
	}
	note := "no matching UpdateLiveryGroup upload"
	if name != "" {
		bid, bdata, err := c.Ugc.BlobByName(r.Xuid, name)
		if err != nil {
			return nil, err
		}
		if bid != 0 {
			if up := ugc.ParseLayergroupUpload(bdata); up != nil {
				if _, err := c.Ugc.VersionedSetData(vid, up.Payload, 0); err != nil {
					return nil, err
				}
				if _, err := c.Ugc.VersionedSetLayers(vid, int64(up.Layers), r.Xuid); err != nil {
					return nil, err
				}
				note = fmt.Sprintf("data %dB, %d layers", len(up.Payload), up.Layers)
			}
		}
	}
	c.Log(r, fmt.Sprintf("  CreateVersionedLayerGroup %s thumb=%dB -> versioned %d (%s)", pyjson.PyRepr(name), len(thumb), vid, note))
	out := xls.NewStruct()
	out.Set("versionedLayerGroupId", vid)
	return out, nil
}

func storeLayerGroupCreate(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	d := req.Sub("layerGroupDescription").Sub("base")
	vid := d.Int("VersionedLayerGroupId")
	fid, err := c.createFile(r, "layergroup", req.Sub("fileHeader"), &vid, nil, req.Str("fileName"), req.Bytes("previewData"), nil)
	if err != nil {
		return nil, err
	}
	out := xls.NewStruct()
	out.Set("contentFileId", fid)
	return out, nil
}

func cvCreateTuneStructs(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	carID := req.Int("carId")
	installed := structToJSON(req.Get("installedParts"))
	bad := c.unbuildableTuneParts(r, carID, installed)
	if len(bad) > 0 {
		c.Warn(r, fmt.Sprintf("  CreateVersionedTuneFromStructs car=%d refused: %d part(s) the game cannot build, first %s (slot, id, why)", carID, len(bad), reprIntegrity(bad[:min(4, len(bad))])))
		return nil, xerr("this tune has parts the game cannot build")
	}
	vid, err := c.Ugc.VersionedCreate("tune", r.Xuid, "", &carID, []byte{}, 0, installed, structToJSON(req.Get("tuning")), nil)
	if err != nil {
		return nil, err
	}
	c.Log(r, fmt.Sprintf("  CreateVersionedTuneFromStructs car=%d -> versioned %d", carID, vid))
	out := xls.NewStruct()
	out.Set("versionedTuneId", vid)
	return out, nil
}

func reprIntegrity(es []parts.IntegrityError) string {
	items := make([]string, len(es))
	for i, e := range es {
		items[i] = fmt.Sprintf("(%d, %d, %s)", e.Slot, e.Word, pyjson.PyRepr(e.Text))
	}
	return "[" + strings.Join(items, ", ") + "]"
}

// layerCount: numberOfLayers is an unsigned 16-bit field on the wire; a design stored with -1 (count unknown) crashed the encoder.
func layerCount(n int64) int64 { return max(0, min(0xFFFF, n)) }

// liveryLayers is the layer count of a stored design. Shares overwrote it with the client's -1 ("unknown"); the real count is in the UpdateLivery upload
// the design came from (same owner and name), so it is read from there. 0 only if that upload is gone.
func (c *Core) liveryLayers(v *ugc.Versioned) int64 {
	if v.Layers >= 0 {
		return layerCount(v.Layers)
	}
	if v.Name != "" {
		if bid, data, err := c.Ugc.BlobByName(v.Creator, v.Name); err == nil && bid != 0 {
			if up := ugc.ParseLiveryUpload(data); up != nil {
				return layerCount(int64(up.Layers))
			}
		}
	}
	return layerCount(0)
}

var shimRe = regexp.MustCompile(`^(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?(-debug)?$`)

const (
	placementFixRC  = 3    // first release candidate whose shim turns the unsupported livery texture into an ordinary one
	liveryGateBytes = 1100 // a design's inflated container: about 1034 bytes for an empty design, 1.5 KB and up with layers
)

// shimHasPlacementFix: true when the client's shim carries patch_placement_fallback: from 1.0.0-rc3 on, any later release, and our own debug builds.
// Without it the game stops when a car wearing a layered livery appears.
func shimHasPlacementFix(version string) bool {
	m := shimRe.FindStringSubmatch(strings.TrimSpace(version))
	if m == nil {
		return false
	}
	major, _ := strconv.Atoi(m[1])
	minor, _ := strconv.Atoi(m[2])
	patch, _ := strconv.Atoi(m[3])
	if m[5] != "" {
		return true
	}
	cmp := func() int { // (major, minor, patch) against (1, 0, 0)
		for i, p := range [3]int{major, minor, patch} {
			if w := [3]int{1, 0, 0}[i]; p != w {
				if p > w {
					return 1
				}
				return -1
			}
		}
		return 0
	}()
	if m[4] == "" {
		return cmp >= 0
	}
	rc, _ := strconv.Atoi(m[4])
	return cmp > 0 || rc >= placementFixRC
}

// designHasLayers: a design with layers, judged by the stored count and, because the count is unreliable (shares stored -1), by the size of the data.
func designHasLayers(storedLayers int64, data []byte) bool {
	if storedLayers > 0 {
		return true
	}
	if len(data) == 0 {
		return false
	}
	container := ugc.LiveryChunk(ugc.StripLiveryPrefix(data))
	return container == nil || len(container) > liveryGateBytes
}

// countDownload: a player fetched the full data of a shared item: one download of its listing, once per player (the creator's own fetches do not count).
func (c *Core) countDownload(r *Req, vid, xuid int64) {
	fid, err := c.Ugc.FileOfVersioned(vid)
	if err != nil || fid == 0 {
		return
	}
	if ok, err := c.Ugc.RecordAction(fid, xuid, "download", "downloads"); err == nil && ok {
		c.Log(r, fmt.Sprintf("  download of file #%d counted for %d", fid, xuid))
	}
}

func cvGetLivery(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	vid := req.Int("versionedLiveryId")
	v, err := c.Ugc.VersionedGet(vid)
	if err != nil {
		return nil, err
	}
	if v == nil || v.Kind != "livery" {
		return nil, xerr("no such livery %d", vid)
	}
	xuid := r.Xuid
	if os.Getenv("FH2MP_LIVERY_GATE_OFF") == "" && v.Creator != xuid && !shimHasPlacementFix(r.Shim) && designHasLayers(v.Layers, v.Data) {
		// A car wearing a layered design makes the game build a 1024x1024 11-mip texture that Azimuth cannot place; clients without the shim fallback stop.
		// They get the answer designs with layers=-1 always got (an error): the car shows without the design, nothing crashes.
		shim := r.Shim
		if shim == "" {
			shim = "unknown"
		}
		c.Log(r, fmt.Sprintf("  RetrieveVersionedLivery %d: withheld from a client without the texture fix (shim %s, stored layers %d, creator %#x, user %#x)", v.ID, shim, v.Layers, v.Creator, xuid))
		return nil, xerr("livery %d is not available to this client", v.ID)
	}
	res := c.defCall("ForzaContentVersioningRetrieveVersionedLiveryById_Results")
	d := res.Sub("versionedLiveryData")
	data := ugc.StripLiveryPrefix(append([]byte{}, v.Data...)) // the client's own compression-stream blob, served as uploaded
	var car int64
	if v.CarID != nil {
		car = *v.CarID
	}
	thumb, small := append([]byte{}, v.Thumb...), append([]byte{}, v.ThumbSmall...)
	if len(thumb) == 0 { // a design shared before thumbnails were generated: derive them from its storefront preview now
		f, err := c.DB.One("SELECT id FROM ugc_files WHERE versioned_id=? AND removed=0 ORDER BY id DESC LIMIT 1", v.ID)
		if err != nil {
			return nil, err
		}
		var pv []byte
		if f != nil {
			if file, err := c.Ugc.FileGet(f.Int("id")); err == nil && file != nil {
				pv = file.Preview
			}
		}
		if len(pv) > 0 {
			if big, sm := c.liveryThumbs(r, pv); len(big) > 0 {
				if _, err := c.Ugc.VersionedSetThumbs(v.ID, big, sm, 0); err != nil {
					return nil, err
				}
				thumb, small = big, sm
			}
		}
	}
	if len(thumb) == 0 && os.Getenv("FH2MP_NO_THUMB_PLACEHOLDER") == "" {
		// A design that was saved but never shared has no thumbnail. Other players' games fetch it when its owner joins and the game expects the
		// thumbnails to be there: serve a plain thumbnail of the right size instead of nothing.
		thumb, small = placeholderThumbs()
	}
	layers := c.liveryLayers(v)
	d.Set("profileId", int64(0))
	d.Set("carId", car)
	d.Set("numberOfLayers", layers)
	d.Set("creatorXuid", v.Creator)
	d.Set("data", data)
	d.Set("liveryThumbnail", thumb)
	d.Set("liveryThumbnailSmall", small)
	c.Log(r, fmt.Sprintf("  RetrieveVersionedLivery %d: %dB data, %d layers, thumb %dB", v.ID, len(data), layers, len(thumb)))
	if req.Int("returnThumbnail") == 0 { // the storefront's download asks for the data alone; a car seen in a lobby asks with its thumbnail
		c.countDownload(r, v.ID, xuid)
	}
	return res, nil
}

func cvGetLayerGroup(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	vid := req.Int("versionedLayerGroupId")
	v, err := c.Ugc.VersionedGet(vid)
	if err != nil {
		return nil, err
	}
	if v == nil || v.Kind != "layergroup" {
		return nil, xerr("no such layer group %d", vid)
	}
	res := c.defCall("ForzaContentVersioningRetrieveVersionedLayerGroupById_Results")
	d := res.Sub("versionedLayerGroupData")
	d.Set("name", v.Name)
	d.Set("numberOfLayers", layerCount(v.Layers))
	d.Set("creatorXuid", v.Creator)
	d.Set("data", append([]byte{}, v.Data...))
	d.Set("thumbnailData", append([]byte{}, v.Thumb...))
	c.Log(r, fmt.Sprintf("  RetrieveVersionedLayerGroup %d: %dB data, %d layers", v.ID, len(v.Data), layerCount(v.Layers)))
	c.countDownload(r, v.ID, r.Xuid)
	return res, nil
}

// unbuildableTuneParts lists the tune's part ids the game data does not have or that belong to another car; empty when the list is fine, the car is one
// the catalog does not know (car packs) or the list has no part array.
func (c *Core) unbuildableTuneParts(r *Req, carID int64, installed any) []parts.IntegrityError {
	obj, ok := installed.(pyjson.Obj)
	if !ok || c.Tables == nil || carID == 0 {
		return nil
	}
	wv, _ := obj.Get("m_CarParts")
	words, ok := wv.([]any)
	if !ok || len(words) != parts.Slots {
		return nil
	}
	if _, ok := parts.StockList(c.Tables, carID); !ok {
		return nil
	}
	w := make([]uint32, len(words))
	for i, x := range words {
		w[i] = uint32(xls.AsInt(x))
	}
	return parts.IntegrityErrors(c.Tables, w)
}

// buildableTuneParts is the tune's installed parts, repaired when the game could not build the car from them. Opening a tune makes the client simulate the
// car for its stats; an engine slot without a catalog row leaves the torque curve empty and that loop reads past its buffer. The part list becomes what the
// lobby check would make of it, or the car's stock parts when that still cannot be built. Cars the catalog does not know and well-formed lists are served
// as stored.
func (c *Core) buildableTuneParts(r *Req, carID int64, installed pyjson.Obj, tuneID int64) pyjson.Obj {
	if len(c.unbuildableTuneParts(r, carID, installed)) == 0 {
		return installed
	}
	stock, _ := parts.StockList(c.Tables, carID)
	wv, _ := installed.Get("m_CarParts")
	var raw []uint32
	for _, x := range wv.([]any) {
		raw = append(raw, uint32(xls.AsInt(x)))
	}
	fixed, _, left := parts.Neutralize(c.Tables, raw)
	if len(left) > 0 || len(parts.IntegrityErrors(c.Tables, fixed)) > 0 {
		fixed = stock
	}
	c.Log(r, fmt.Sprintf("  tune %d of car %d has parts the game cannot build; served repaired", tuneID, carID))
	out := append(pyjson.Obj(nil), installed...)
	signed := make([]any, len(fixed))
	for i, w := range fixed {
		if w >= 1<<31 {
			signed[i] = int64(w) - (1 << 32)
		} else {
			signed[i] = int64(w)
		}
	}
	out.Set("m_CarParts", signed)
	return out
}

func cvGetTune(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	vid := req.Int("versionedTuneId")
	v, err := c.Ugc.VersionedGet(vid)
	if err != nil {
		return nil, err
	}
	if v == nil || v.Kind != "tune" {
		return nil, xerr("no such tune %d", vid)
	}
	res := c.defCall("ForzaContentVersioningRetrieveVersionedTuneById_Results")
	d := res.Sub("versionedTuneData")
	var car int64
	if v.CarID != nil {
		car = *v.CarID
	}
	name := c.Tag(v.Creator)
	if name == "" {
		name = c.tagFromDB(v.Creator)
	}
	d.Set("CarId", car)
	d.Set("CreatorName", name)
	d.Set("CreatorXuid", v.Creator)
	if v.Parts != nil && *v.Parts != "" {
		pv, err := pyjson.Parse(*v.Parts)
		if err != nil {
			return nil, err
		}
		if po, ok := pv.(pyjson.Obj); ok {
			pv = c.buildableTuneParts(r, car, po, vid)
		}
		d.Set("InstalledParts", jsonToStruct(pv))
	}
	if v.Tuning != nil && *v.Tuning != "" {
		tv, err := pyjson.Parse(*v.Tuning)
		if err != nil {
			return nil, err
		}
		d.Set("Tuning", jsonToStruct(tv))
	}
	c.countDownload(r, v.ID, r.Xuid)
	return res, nil
}

func cvRef(field string, delta int64) handlerFn {
	return func(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
		return nil, c.Ugc.VersionedRef(req.Int(field), delta)
	}
}

// createFile lists a design / tune / vinyl group in the storefront.
func (c *Core) createFile(r *Req, kind string, header *xls.Struct, versionedID *int64, carID *int64, fileName string, preview []byte, meta *ugc.FileSpec) (int64, error) {
	xuid := r.Xuid
	var blobID int64
	var blobData []byte
	if fileName != "" {
		var err error
		if blobID, blobData, err = c.Ugc.BlobByName(xuid, fileName); err != nil {
			return 0, err
		}
	}
	var v *ugc.Versioned
	if versionedID != nil && *versionedID != 0 {
		var err error
		if v, err = c.Ugc.VersionedGet(*versionedID); err != nil {
			return 0, err
		}
	}
	if v != nil && v.Creator != xuid {
		c.Warn(r, fmt.Sprintf("  %s share refused: versioned item %d belongs to %#x, not to %#x", kind, *versionedID, v.Creator, xuid))
		return 0, xerr("versioned item %d is not yours", *versionedID)
	}
	if carID == nil || *carID == 0 { // a tune's listing carries make/model/class only; its car is on the versioned item
		carID = nil
		if v != nil {
			carID = v.CarID
		}
	}
	if kind == "livery" && v != nil {
		if blobID != 0 && len(v.Data) == 0 {
			if _, err := c.Ugc.VersionedSetData(*versionedID, blobData, xuid); err != nil {
				return 0, err
			}
		}
		if q := header.Int("Quantity"); q > 0 { // the client sends -1 when it does not know; that must not replace a real count
			if _, err := c.Ugc.VersionedSetLayers(*versionedID, q, xuid); err != nil {
				return 0, err
			}
		}
	}
	spec := ugc.FileSpec{}
	if carID != nil && *carID != 0 {
		row, err := c.DB.One("SELECT make_id, model_id, class_id FROM game.v_cars WHERE id=?", *carID)
		if err != nil {
			return 0, err
		}
		if row != nil {
			mk, md, cl := row.Int("make_id"), row.Int("model_id"), row.Int("class_id")
			spec.MakeID, spec.ModelID, spec.ClassID = &mk, &md, &cl
		}
	}
	if meta != nil {
		if meta.MakeID != nil {
			spec.MakeID = meta.MakeID
		}
		if meta.ModelID != nil {
			spec.ModelID = meta.ModelID
		}
		if meta.ClassID != nil {
			spec.ClassID = meta.ClassID
		}
		if meta.Env != nil {
			spec.Env = meta.Env
		}
		if meta.Track != nil {
			spec.Track = meta.Track
		}
		if meta.PI != nil {
			spec.PI = meta.PI
		}
	}
	if kind == "layergroup" && v != nil && blobID != 0 && len(v.Data) == 0 { // normally attached when CreateVersionedLayerGroup ran
		if up := ugc.ParseLayergroupUpload(blobData); up != nil {
			if _, err := c.Ugc.VersionedSetData(*versionedID, up.Payload, xuid); err != nil {
				return 0, err
			}
			if _, err := c.Ugc.VersionedSetLayers(*versionedID, int64(up.Layers), xuid); err != nil {
				return 0, err
			}
		}
	}
	fileKind.Store(header.Int("Type"), kind)
	if kind == "livery" && v != nil && len(preview) >= 4 && string(preview[:4]) == "\x89PNG" {
		if big, small := c.liveryThumbs(r, preview); len(big) > 0 {
			if _, err := c.Ugc.VersionedSetThumbs(*versionedID, big, small, xuid); err != nil {
				return 0, err
			}
		}
	}
	spec.Kind, spec.Owner, spec.Title, spec.Description, spec.Type = kind, xuid, header.Str("Title"), header.Str("Description"), header.Int("Type")
	spec.VersionedID, spec.PayloadSize, spec.Preview, spec.CarID = versionedID, header.Int("PayloadSize"), append([]byte{}, preview...), carID
	if blobID != 0 {
		spec.BlobID = &blobID
	}
	spec.Keyword1, spec.Keyword2 = header.Int("KeywordOne"), header.Int("KeywordTwo")
	fid, err := c.Ugc.FileCreate(spec)
	if err != nil {
		return 0, err
	}
	blob := "None"
	if blobID != 0 {
		blob = fmt.Sprintf("#%d", blobID)
	}
	vs := "None"
	if versionedID != nil {
		vs = fmt.Sprint(*versionedID)
	}
	c.Log(r, fmt.Sprintf("  %s shared: file #%d %s type=%d versioned=%s payload=%d blob=%s preview=%dB", kind, fid, pyjson.PyRepr(header.Str("Title")), header.Int("Type"), vs,
		header.Int("PayloadSize"), blob, len(preview)))
	return fid, nil
}

func storeLiveryCreate(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	d := req.Sub("liveryDescription")
	vid, car := d.Sub("base").Int("VersionedLiveryId"), d.Int("carId")
	fid, err := c.createFile(r, "livery", req.Sub("fileHeader"), &vid, &car, req.Str("fileName"), req.Bytes("previewData"), nil)
	if err != nil {
		return nil, err
	}
	out := xls.NewStruct()
	out.Set("contentFileId", fid)
	return out, nil
}

func storeTuningCreate(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	d := req.Sub("tuningDescription").Sub("base")
	mk, md, cl, env, tr, pi := d.Int("MakeId"), d.Int("ModelId"), d.Int("ClassId"), d.Int("EnvironmentId"), d.Int("TrackId"), d.Int("PerformanceIndex")
	meta := &ugc.FileSpec{MakeID: &mk, ModelID: &md, ClassID: &cl, Env: &env, Track: &tr, PI: &pi}
	vid := d.Int("VersionedTuneId")
	fid, err := c.createFile(r, "tune", req.Sub("fileHeader"), &vid, nil, req.Str("fileName"), req.Bytes("previewData"), meta)
	if err != nil {
		return nil, err
	}
	out := xls.NewStruct()
	out.Set("contentFileId", fid)
	return out, nil
}

// storePhotoCreate lists a photo (title, keywords, preview) and returns its file id; the client then uploads the full picture with UploadContent under that
// id, and the photo is searchable once the upload is complete.
func storePhotoCreate(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	h, d := req.Sub("fileHeader"), req.Sub("photoDescription").Sub("base")
	bid, err := c.Ugc.BlobCreate(r.Xuid, "", req.Int("payloadSize"))
	if err != nil {
		return nil, limitErr(err)
	}
	spec := ugc.FileSpec{Kind: "photo", Owner: r.Xuid, Title: h.Str("Title"), Description: h.Str("Description"), Type: h.Int("Type"), PayloadSize: req.Int("payloadSize"),
		Preview: append([]byte{}, req.Bytes("previewData")...), BlobID: &bid, Keyword1: h.Int("KeywordOne"), Keyword2: h.Int("KeywordTwo")}
	if v := d.Int("MakeId"); v != 0 {
		spec.MakeID = &v
	}
	if v := d.Int("ModelId"); v != 0 {
		spec.ModelID = &v
	}
	env, track := d.Int("EnvironmentId"), d.Int("TrackId")
	spec.Env, spec.Track = &env, &track
	fid, err := c.Ugc.FileCreate(spec)
	if err != nil {
		return nil, err
	}
	fileKind.Store(h.Int("Type"), "photo")
	c.Log(r, fmt.Sprintf("  photo shared: file #%d %s type=%d make=%d model=%d payload=%d preview=%dB (upload as content %d, blob #%d)", fid, pyjson.PyRepr(h.Str("Title")), h.Int("Type"),
		d.Int("MakeId"), d.Int("ModelId"), req.Int("payloadSize"), len(req.Bytes("previewData")), fid, bid))
	out := xls.NewStruct()
	out.Set("contentFileId", fid)
	return out, nil
}

// shown filters community files this reader may be listed. A tune, livery or photo of a car the server's car database does not have (a player's own car
// pack) is hidden from everyone who does not run their own database: the game would try to show a car it has not got.
func (c *Core) shown(r *Req, files []*ugc.File) []*ugc.File {
	if r.OwnCars || c.Cars == nil {
		return files
	}
	known := map[int64]bool{}
	var out []*ugc.File
	for _, f := range files {
		if f.CarID == nil || *f.CarID <= 0 {
			out = append(out, f)
			continue
		}
		cid := *f.CarID
		if _, ok := known[cid]; !ok {
			row, _ := c.Cars.Row(cid)
			known[cid] = row != nil
		}
		if known[cid] {
			out = append(out, f)
		}
	}
	return out
}

func listLimit(n, top, dflt int64) int {
	v := min(max(n, 0), top)
	if v == 0 {
		v = dflt
	}
	return int(v)
}

func (c *Core) searchFiles(r *Req, kind, resultsStruct, label string, req *xls.Struct) (*xls.Struct, error) {
	p := req.Sub("searchParameters")
	cls := p.Int("ClassId")
	files, err := c.Ugc.Search(ugc.SearchSpec{Kind: kind, Make: p.Int("MakeId"), Model: p.Int("ModelId"), Class: &cls, Kw1: p.Int("KeywordOneId"), Kw2: p.Int("KeywordTwoId"),
		Env: p.Int("EnvironmentId"), Track: p.Int("TrackId"), Layers: p.Int("NumberOfLayers"), Gamertag: p.Str("GamerTag"), Text: p.Str("ManualSearchKeywords"),
		Limit: listLimit(req.Int("maxFiles"), 100, 50)})
	if err != nil {
		return nil, err
	}
	files = c.shown(r, files)
	tmpl := c.elemTemplate(resultsStruct, "searchResults")
	c.Log(r, fmt.Sprintf("  %s make=%d model=%d class=%d kw=%d/%d env=%d track=%d tag=%s text=%s: %d found", label, p.Int("MakeId"), p.Int("ModelId"), cls, p.Int("KeywordOneId"),
		p.Int("KeywordTwoId"), p.Int("EnvironmentId"), p.Int("TrackId"), pyjson.PyRepr(p.Str("GamerTag")), pyjson.PyRepr(p.Str("ManualSearchKeywords")), len(files)))
	rows := []any{}
	for _, f := range files {
		rows = append(rows, c.light(f, tmpl))
	}
	out := xls.NewStruct()
	out.Set("searchResults", rows)
	return out, nil
}

const photoPicksDays = 7 // "picks" tab: the most liked photos shared in the last week (all time when there are none)

func (c *Core) photoList(r *Req, resultsStruct, field, label string, files []*ugc.File, req *xls.Struct) (*xls.Struct, error) {
	tmpl := c.elemTemplate(resultsStruct, field)
	c.Log(r, fmt.Sprintf("  %s max=%d: %d photos", label, req.Int("maxFiles"), len(files)))
	rows := []any{}
	for _, f := range files {
		full, err := c.Ugc.FileGet(f.ID) // FileGet unpacks the preview
		if err != nil {
			return nil, err
		}
		rows = append(rows, c.heavy(full, tmpl))
	}
	out := xls.NewStruct()
	out.Set(field, rows)
	return out, nil
}

// storePopularPhotos is Storefront.GetPopularPhotos (the "Top Rated" tab).
func storePopularPhotos(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	files, err := c.Ugc.Popular("photo", listLimit(req.Int("maxFiles"), 100, 50), 0)
	if err != nil {
		return nil, err
	}
	return c.photoList(r, "ForzaStorefrontGetPopularPhotos_Results", "PopularPhotos", "GetPopularPhotos", c.shown(r, files), req)
}

// storeFeaturedPhotos is Storefront.GetFeaturedPhotos (the curated tab): nobody curates, so the week's most liked photos stand in.
func storeFeaturedPhotos(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	n := listLimit(req.Int("maxFiles"), 100, 50)
	files, err := c.Ugc.Popular("photo", n, c.Now()-photoPicksDays*86400)
	if err != nil {
		return nil, err
	}
	if len(files) == 0 {
		if files, err = c.Ugc.Popular("photo", n, 0); err != nil {
			return nil, err
		}
	}
	return c.photoList(r, "ForzaStorefrontGetFeaturedPhotos_Results", "FeaturedPhotos", "GetFeaturedPhotos", c.shown(r, files), req)
}

func storeHeavy(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	var ids []int64
	for _, v := range req.List("fileIds") {
		ids = append(ids, xls.AsInt(v))
	}
	if n := req.Int("fileCount"); n != 0 {
		ids = ids[:min(len(ids), int(max(0, n)))]
	}
	files, err := c.Ugc.Files(ids)
	if err != nil {
		return nil, err
	}
	tmpl := c.elemTemplate("ForzaStorefrontGetFileHeavyWeightData_Results", "fileData")
	l := make([]string, len(ids))
	for i, id := range ids {
		l[i] = fmt.Sprint(id)
	}
	c.Log(r, fmt.Sprintf("  GetFileHeavyWeightData [%s]: %d found", strings.Join(l, ", "), len(files)))
	rows := []any{}
	for _, f := range files {
		rows = append(rows, c.heavy(f, tmpl))
	}
	out := xls.NewStruct()
	out.Set("fileData", rows)
	return out, nil
}

func storeMyFiles(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	files, err := c.Ugc.Mine(r.Xuid, req.Int("fileType"), listLimit(req.Int("maxFiles"), 500, 100))
	if err != nil {
		return nil, err
	}
	tmpl := c.elemTemplate("ForzaStorefrontGetMyFileList_Results", "files")
	rows := []any{}
	for _, f := range files {
		row := tmpl.Clone()
		row.Set("Id", f.ID)
		row.Set("Title", f.Title)
		row.Set("Description", f.Description)
		row.Set("SubmissionTime", iso(f.Created))
		row.Set("IsUnderReview", false)
		var vid int64
		if f.VersionedID != nil {
			vid = *f.VersionedID
		}
		row.Set("VersionedContentId", vid)
		row.Set("PopularityRating", int64(f.Stars()))
		row.Set("Ordinal", ordinal(f))
		row.Set("DownloadCount", f.Downloads)
		row.Set("LikeCount", f.Likes)
		row.Set("UseCount", f.Uses)
		rows = append(rows, row)
	}
	c.Log(r, fmt.Sprintf("  GetMyFileList type=%d: %d files", req.Int("fileType"), len(rows)))
	out := xls.NewStruct()
	out.Set("files", rows)
	return out, nil
}

func storeIfShared(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	row, err := c.DB.One("SELECT id FROM ugc_files WHERE versioned_id=? AND removed=0 ORDER BY id DESC LIMIT 1", req.Int("versionedItemId"))
	if err != nil {
		return nil, err
	}
	res := c.defCall("ForzaStorefrontGetFileDataIfShared_Results")
	if row != nil {
		f, err := c.Ugc.FileGet(row.Int("id"))
		if err != nil {
			return nil, err
		}
		tmpl := c.defCall("ForzaStorefrontGetFileDataIfShared_Results").Sub("storefrontData")
		res.Set("found", true)
		res.Set("storefrontData", c.light(f, tmpl))
	}
	return res, nil
}

// bumpHandler is IncrementViews / IncrementUsage (a counter per call) and IncrementLikes (one like per player and file; the game remembers a like in the
// profile, so a repeat only comes from a reinstalled or second profile). The client sends them as fire-and-forget calls.
func bumpHandler(column string) handlerFn {
	return func(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
		fid := req.Int("fileId")
		if column == "likes" {
			counted, err := c.Ugc.RecordAction(fid, r.Xuid, "like", "likes")
			if err != nil {
				return nil, err
			}
			what := "not counted (repeat, own file or unknown file)"
			if counted {
				what = "counted"
			}
			c.Log(r, fmt.Sprintf("  IncrementLikes file #%d by %d: %s", fid, r.Xuid, what))
		} else {
			if err := c.Ugc.Bump(fid, column); err != nil {
				return nil, err
			}
			c.Log(r, fmt.Sprintf("  Increment%s file #%d", strings.ToUpper(column[:1])+column[1:], fid))
		}
		return nil, nil
	}
}

func storeRemove(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	fid := req.Int("fileId")
	n, err := c.Ugc.Remove(fid, r.Xuid)
	if err != nil {
		return nil, err
	}
	what := "not yours / unknown"
	if n > 0 {
		what = "removed"
	}
	c.Log(r, fmt.Sprintf("  RemoveFile #%d: %s", fid, what))
	return nil, nil
}

// featuredPick is the featured liveries' choice: at most n, one per car, in the order given. A livery without a car is left out: the game
// shows it as a car card with no car, and its drivetrain converter reads NULL for that card (#412, game+0xb99cd3).
func featuredPick(files []*ugc.File, n int) []*ugc.File {
	seen := map[int64]bool{}
	var out []*ugc.File
	for _, f := range files {
		if len(out) >= n {
			break
		}
		if f.CarID == nil || *f.CarID <= 0 || seen[*f.CarID] {
			continue
		}
		seen[*f.CarID] = true
		out = append(out, f)
	}
	return out
}

func storeFeaturedLiveries(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	all, err := c.Ugc.Search(ugc.SearchSpec{Kind: "livery", Limit: 200})
	if err != nil {
		return nil, err
	}
	files := featuredPick(c.shown(r, all), int(max(1, req.Int("numCars"))))
	tmpl := c.elemTemplate("ForzaStorefrontGetFeaturedLiveriesForRandomCars_Results", "liveryData")
	rows := []any{}
	for _, f := range files {
		full, err := c.Ugc.FileGet(f.ID) // FileGet unpacks the preview
		if err != nil {
			return nil, err
		}
		rows = append(rows, c.heavy(full, tmpl))
	}
	out := xls.NewStruct()
	out.Set("liveryData", rows)
	return out, nil
}

func storeDrivatarLiveries(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	rows, err := c.DB.Query("SELECT id, versioned_id FROM ugc_files WHERE kind='livery' AND car_id=? AND removed=0 AND versioned_id IS NOT NULL "+
		"ORDER BY likes DESC, id DESC LIMIT ?", req.Int("carId"), max(1, req.Int("maxContentResults")))
	if err != nil {
		return nil, err
	}
	out := []any{}
	for i := range rows {
		s := xls.NewStruct()
		s.Set("Id", rows[i].Int("id"))
		s.Set("VersionedContentId", rows[i].Int("versioned_id"))
		out = append(out, s)
	}
	res := xls.NewStruct()
	res.Set("drivatarLiveries", out)
	return res, nil
}

// storeCurated is Storefront.GetCuratedContent (the "Recommended Designs" page): community files of the asked type for the player's car.
func storeCurated(c *Core, r *Req, req *xls.Struct) (*xls.Struct, error) {
	p := req.Sub("curationParameters")
	files, err := c.Ugc.Curated(p.Int("ContentType"), p.Int("CarId"), int(min(max(req.Int("maxContentResults"), 1), 100)))
	if err != nil {
		return nil, err
	}
	files = c.shown(r, files)
	tmpl := c.elemTemplate("ForzaStorefrontGetCuratedContent_Results", "curatedContent")
	c.Log(r, fmt.Sprintf("  GetCuratedContent type=%d car=%d track=%d pi=%d: %d found", p.Int("ContentType"), p.Int("CarId"), p.Int("TrackId"), p.Int("PiTargetValue"), len(files)))
	rows := []any{}
	for _, f := range files {
		rows = append(rows, c.light(f, tmpl))
	}
	out := xls.NewStruct()
	out.Set("curatedContent", rows)
	return out, nil
}

var _ = diag.Info
