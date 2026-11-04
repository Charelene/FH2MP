// Package ugc is the user-generated content store: paint jobs (liveries), tunes, vinyl groups, photos, with likes and downloads
// (= server/ugc.py). Large blobs are uploaded in chunks, versioned items (livery / layer group / tune) are immutable, and the storefront
// file is the public listing other players search. Everything is stored in the state database; large values are LZMA-packed.
package ugc

import (
	"bytes"
	"compress/zlib"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"strconv"
	"strings"
	"time"

	"github.com/ulikunitz/xz/lzma"

	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
)

var (
	log  = diag.Get("ugc")
	rate = diag.NewRateLimit(nil, 0)
)

// Limits.
const (
	DamagedLogSeconds = 600.0
	MaxRequestBody    = 16 << 20 // largest HTTP body the LSP reads from a game client
	MaxBlobSize       = 8 << 20  // one uploaded blob, declared total size
	MaxChunk          = MaxBlobSize
	MaxLiveryRaw      = 16 << 20 // a livery's inflated container
	MaxBlobsPerOwner  = 400
	MaxBytesPerOwner  = 256 << 20 // declared sizes of one account's blobs, together
	MaxUnpacked       = 32 << 20  // largest value Unpack will expand
	PhotoMaxDim       = 8192
	PhotoQuality      = 90
	PhotoTimeout      = 30 * time.Second
)

// Magic prefixes a packed value: Magic + codec byte (0 = stored, 1 = LZMA) + payload; rows without it are legacy raw bytes.
var Magic = []byte("\x89FZ")

// ContentKinds are the storefront kinds whose payload is uploaded with UploadContent under the file id.
var ContentKinds = []string{"photo", "replay"}

// PhotoSize is the Xbox One's native photo size: the game shows a photo of any other size cropped.
var PhotoSize = [2]int{1920, 1080}

// LimitError is an upload that breaks a size, offset or quota limit.
type LimitError struct{ Msg string }

func (e *LimitError) Error() string { return e.Msg }

func limitf(f string, a ...any) error { return &LimitError{fmt.Sprintf(f, a...)} }

// JpegSize is (width, height) from the first start-of-frame marker of a JPEG; ok=false for anything that is not a well-formed JPEG header.
func JpegSize(data []byte) (w, h int, ok bool) {
	if len(data) < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF {
		return 0, 0, false
	}
	i := 2
	for i+4 <= len(data) {
		if data[i] != 0xFF {
			return 0, 0, false
		}
		m := data[i+1]
		if m == 0xFF {
			i++
			continue
		}
		switch m {
		case 0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF:
			if i+9 > len(data) {
				return 0, 0, false
			}
			hh, ww := int(binary.BigEndian.Uint16(data[i+5:])), int(binary.BigEndian.Uint16(data[i+7:]))
			if ww == 0 || hh == 0 {
				return 0, 0, false
			}
			return ww, hh, true
		}
		if m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) {
			i += 2
			continue
		}
		i += 2 + int(binary.BigEndian.Uint16(data[i+2:]))
	}
	return 0, 0, false
}

// Photo1080p is a photo upload as 1920x1080 JPEG (other aspect ratios are scaled to cover the frame and cut in the middle). It returns data
// unchanged when it already has that size, is not a JPEG (or is implausibly large) or ImageMagick is missing or fails.
func Photo1080p(data []byte) []byte {
	w, h, ok := JpegSize(data)
	if !ok || [2]int{w, h} == PhotoSize || max(w, h) > PhotoMaxDim {
		return data
	}
	tool, err := exec.LookPath("magick")
	if err != nil {
		tool, err = exec.LookPath("convert")
	}
	if err != nil {
		if ok, skipped := rate.Allow("photo-no-tool", DamagedLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (%d photos since the last line)", skipped)
			}
			log.Warn(fmt.Sprintf("photo stays %dx%d instead of %dx%d: ImageMagick (magick / convert) is not installed, so the game shows it cropped%s", w, h, PhotoSize[0], PhotoSize[1], extra))
		}
		return data
	}
	pw, ph := PhotoSize[0], PhotoSize[1]
	ctx, cancel := context.WithTimeout(context.Background(), PhotoTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, tool, "-limit", "memory", "512MiB", "-limit", "map", "512MiB", "-limit", "disk", "0", "jpeg:-", "-auto-orient", "-strip",
		"-resize", fmt.Sprintf("%dx%d^", pw, ph), "-gravity", "center", "-extent", fmt.Sprintf("%dx%d", pw, ph), "-quality", strconv.Itoa(PhotoQuality), "jpeg:-")
	cmd.Stdin = bytes.NewReader(data)
	var out bytes.Buffer
	cmd.Stdout = &out
	if err := cmd.Run(); err != nil {
		log.Warn(fmt.Sprintf("photo %dx%d (%d bytes) not converted to %dx%d, stored as uploaded: %v", w, h, len(data), pw, ph, err))
		return data
	}
	ow, oh, ook := JpegSize(out.Bytes())
	if ook && [2]int{ow, oh} == PhotoSize && out.Len() <= MaxBlobSize {
		return out.Bytes()
	}
	got := "None"
	if ook {
		got = fmt.Sprintf("(%d, %d)", ow, oh)
	}
	log.Warn(fmt.Sprintf("photo %dx%d (%d bytes) not converted: the converter produced %s in %d bytes (wanted (%d, %d) within %d bytes), stored as uploaded", w, h, len(data), got, out.Len(), pw, ph, MaxBlobSize))
	return data
}

func dictCap(n int) int {
	c := 1 << 12
	for c < n && c < 64<<20 {
		c <<= 1
	}
	return c
}

// Pack compresses a stored blob as hard as it goes (LZMA); keeps the raw bytes when that does not help. Always encodes: bytes that already start with
// Magic are client data, not our storage format, and get wrapped like any other.
func Pack(b []byte) []byte {
	if len(b) == 0 {
		return []byte{}
	}
	var buf bytes.Buffer
	w, err := lzma.WriterConfig{DictCap: dictCap(len(b)), Size: int64(len(b))}.NewWriter(&buf)
	if err == nil {
		_, err = w.Write(b)
		if err == nil {
			err = w.Close()
		}
	}
	if err == nil && buf.Len()+4 < len(b) {
		return append(append(append([]byte(nil), Magic...), 1), buf.Bytes()...)
	}
	return append(append(append([]byte(nil), Magic...), 0), b...)
}

// PackStored is Pack without compressing: for a blob that is still being uploaded.
func PackStored(b []byte) []byte {
	if len(b) == 0 {
		return []byte{}
	}
	return append(append(append([]byte(nil), Magic...), 0), b...)
}

// Unpack is the inverse of Pack (rows without Magic are legacy raw bytes). The expansion is capped at MaxUnpacked.
func Unpack(b []byte) ([]byte, error) {
	if len(b) == 0 {
		return []byte{}, nil
	}
	if len(b) < 4 || !bytes.Equal(b[:3], Magic) {
		return b, nil
	}
	switch b[3] {
	case 0:
		return b[4:], nil
	case 1:
	default:
		return nil, limitf("stored value has an unknown codec %d", b[3])
	}
	// Python's decompressor waits for more input when the stream is cut short and returns what it has decoded: so does this.
	r, err := lzma.NewReader(bytes.NewReader(b[4:]))
	if err != nil {
		if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
			return []byte{}, nil
		}
		return nil, limitf("stored value is corrupt: %v", err)
	}
	out, err := io.ReadAll(io.LimitReader(r, MaxUnpacked+1))
	if err != nil && !errors.Is(err, io.ErrUnexpectedEOF) {
		return nil, limitf("stored value is corrupt: %v", err)
	}
	if len(out) > MaxUnpacked {
		return nil, limitf("stored value expands beyond the limit")
	}
	return out, nil
}

func structErr(need int) string {
	return fmt.Sprintf("error: unpack requires a buffer of %d bytes", need)
}

// liveryChunk is (inflated container, "") or (nil, why it is not one).
func liveryChunk(blob []byte) ([]byte, string) {
	if len(blob) < 8 {
		return nil, fmt.Sprintf("blob is %d bytes, shorter than the 8 byte chunk header", len(blob))
	}
	comp, raw := int(binary.LittleEndian.Uint32(blob)), int(binary.LittleEndian.Uint32(blob[4:]))
	if len(blob) != 8+comp || raw > MaxLiveryRaw {
		return nil, fmt.Sprintf("header says %d compressed bytes (blob has %d) and %d raw bytes (limit %d)", comp, len(blob)-8, raw, MaxLiveryRaw)
	}
	zr, err := zlib.NewReader(bytes.NewReader(blob[8:]))
	if err != nil {
		return nil, fmt.Sprintf("zlib: %v", err)
	}
	out := make([]byte, 0, min(raw+1, 1<<20))
	buf := make([]byte, 32<<10)
	eof := false
	for len(out) < raw+1 {
		n, err := zr.Read(buf[:min(len(buf), raw+1-len(out))])
		out = append(out, buf[:n]...)
		if err == io.EOF {
			eof = true
			break
		}
		if err == io.ErrUnexpectedEOF {
			break
		}
		if err != nil {
			return nil, fmt.Sprintf("zlib: %v", err)
		}
	}
	if len(out) == raw && eof {
		return out, ""
	}
	why := fmt.Sprintf("inflates to %d bytes, the header says %d", len(out), raw)
	if !eof {
		why += " and the zlib stream does not end"
	}
	return nil, why
}

// LiveryChunk checks a livery blob in the game's compression-stream form (one chunk = u32 LE compressed size + u32 LE raw size + that many
// zlib bytes). It returns the inflated 'vlrc' container or nil. The inflate never produces more than the declared raw size.
func LiveryChunk(blob []byte) []byte {
	out, _ := liveryChunk(blob)
	return out
}

// StripLiveryPrefix drops the 13 bytes rows stored before 2026-10-18 kept in front of the blob (flag, creator xuid, blob length).
func StripLiveryPrefix(data []byte) []byte {
	if len(data) > 13 && LiveryChunk(data) == nil && int(binary.BigEndian.Uint32(data[9:13])) == len(data)-13 {
		return data[13:]
	}
	return data
}

// LayerGroupUpload is a parsed OnlineProfile.UpdateLiveryGroup body.
type LayerGroupUpload struct {
	Name    string
	Layers  int
	Payload []byte
}

func slice(b []byte, a, z int) []byte {
	if a > len(b) {
		a = len(b)
	}
	if z > len(b) {
		z = len(b)
	}
	return b[a:z]
}

// ParseLayergroupUpload reads an UpdateLiveryGroup body: u32 name length + name, u16 layer count, u8 flag, u64 creator xuid, then the blob (u32 length
// + bytes). nil when the body does not fit that layout.
func ParseLayergroupUpload(body []byte) *LayerGroupUpload {
	fail := func(need int) *LayerGroupUpload {
		log.Warn(fmt.Sprintf("UpdateLiveryGroup body (%d bytes) does not follow the recovered layout: %s", len(body), structErr(need)))
		return nil
	}
	if len(body) < 4 {
		return fail(4)
	}
	n := int(binary.BigEndian.Uint32(body))
	name := pyfmt.DecodeReplace(slice(body, 4, 4+n))
	if len(name) > 4096 {
		log.Warn(fmt.Sprintf("UpdateLiveryGroup body (%d bytes) refused: name of %d bytes (limit 4096)", len(body), len(name)))
		return nil
	}
	lb := slice(body, 4+n, 6+n)
	if len(lb) < 2 {
		return fail(2)
	}
	layers := int(binary.BigEndian.Uint16(lb))
	p := 6 + n + 1 + 8
	pb := slice(body, p, p+4)
	if len(pb) < 4 {
		return fail(4)
	}
	ln := int(binary.BigEndian.Uint32(pb))
	blob := slice(body, p+4, p+4+ln)
	if len(blob) != ln || ln == 0 {
		log.Warn(fmt.Sprintf("UpdateLiveryGroup body (%d bytes, name %s) is not a vinyl group: %d bytes declared, %d present", len(body), pyfmt.ReprStr(trunc(name, 30)), ln, len(blob)))
		return nil
	}
	return &LayerGroupUpload{name, layers, blob}
}

func trunc(s string, n int) string {
	r := []rune(s)
	if len(r) > n {
		return string(r[:n])
	}
	return s
}

// LiveryUpload is a parsed OnlineProfile.UpdateLivery body.
type LiveryUpload struct {
	Name      string
	CarID     int64
	Layers    int
	Container []byte
	Payload   []byte
}

// ParseLiveryUpload reads an UpdateLivery body: u32 name length + name, i32 carId, u16 layer count, u8 flag, u64 creator xuid, then a blob (u32 BE
// length + bytes) in the compression-stream form of LiveryChunk. Payload is that bare blob. nil when the body does not fit.
func ParseLiveryUpload(body []byte) *LiveryUpload {
	fail := func(need int) *LiveryUpload {
		log.Warn(fmt.Sprintf("UpdateLivery body (%d bytes) does not follow the recovered layout: %s", len(body), structErr(need)))
		return nil
	}
	if len(body) < 4 {
		return fail(4)
	}
	n := int(binary.BigEndian.Uint32(body))
	name := pyfmt.DecodeReplace(slice(body, 4, 4+n))
	cb := slice(body, 4+n, 4+n+6)
	if len(cb) < 6 {
		return fail(6)
	}
	carID, layers := int64(binary.BigEndian.Uint32(cb)), int(binary.BigEndian.Uint16(cb[4:]))
	p := 4 + n + 6 + 1 + 8
	pb := slice(body, p, p+4)
	if len(pb) < 4 {
		return fail(4)
	}
	ln := int(binary.BigEndian.Uint32(pb))
	blob := slice(body, p+4, p+4+ln)
	container, why := liveryChunk(blob)
	if container == nil {
		log.Warn(fmt.Sprintf("UpdateLivery body (%d bytes, name %s, car %d) is not a livery: the blob of %d declared bytes (%d present) %s", len(body), pyfmt.ReprStr(trunc(name, 30)), carID, ln, len(blob), why))
		return nil
	}
	return &LiveryUpload{name, carID, layers, container, blob}
}

func completeSQL() string {
	q := make([]string, len(ContentKinds))
	for i, k := range ContentKinds {
		q[i] = "'" + k + "'"
	}
	return "(f.blob_id IS NULL OR f.kind NOT IN (" + strings.Join(q, ", ") + ") " +
		"OR EXISTS (SELECT 1 FROM ugc_blobs b WHERE b.id = f.blob_id AND b.received >= b.total_size))"
}

// Complete is the filter that hides a listed file whose payload is uploaded after the listing (photos) until the upload is complete.
var Complete = completeSQL()

// ListCols are the ugc_files columns except `preview` (a packed blob): list queries never carry it.
const ListCols = "id, kind, type, owner, title, description, created, payload_size, versioned_id, blob_id, car_id, make_id, model_id, " +
	"class_id, keyword1, keyword2, env, track, pi, downloads, likes, uses, views, popularity, removed"

var listColsF = func() string {
	parts := strings.Split(ListCols, ",")
	for i := range parts {
		parts[i] = "f." + strings.TrimSpace(parts[i])
	}
	return strings.Join(parts, ", ")
}()

// ScoreSQL is how well a file is liked, for ordering lists and the star rating: a like says the most, a use less, a download least.
const ScoreSQL = "(f.likes * 3 + f.uses * 2 + f.downloads)"

// StarSteps are the score steps of the 0-5 star rating.
var StarSteps = [5]int64{1, 6, 20, 60, 150}

// File is one storefront listing.
type File struct {
	ID, Type, Owner                                      int64
	Kind, Title, Description                             string
	Created                                              float64
	PayloadSize                                          int64
	VersionedID, BlobID, CarID, MakeID, ModelID, ClassID *int64
	Keyword1, Keyword2                                   int64
	Env, Track, PI                                       *int64
	Downloads, Likes, Uses, Views, Popularity, Removed   int64
	Preview                                              []byte // only filled by FileGet / Files
}

// Stars is the 0-5 PopularityRating the client shows: one more star at each step of the like/use/download score.
func (f *File) Stars() int {
	score := f.Likes*3 + f.Uses*2 + f.Downloads
	n := 0
	for _, t := range StarSteps {
		if score >= t {
			n++
		}
	}
	return n
}

func optInt(r *db.Row, col string) *int64 {
	if r.IsNull(col) {
		return nil
	}
	v := r.Int(col)
	return &v
}

func fileOf(r *db.Row) *File {
	return &File{ID: r.Int("id"), Kind: r.Str("kind"), Type: r.Int("type"), Owner: r.Int("owner"), Title: r.Str("title"), Description: r.Str("description"),
		Created: r.Float("created"), PayloadSize: r.Int("payload_size"), VersionedID: optInt(r, "versioned_id"), BlobID: optInt(r, "blob_id"),
		CarID: optInt(r, "car_id"), MakeID: optInt(r, "make_id"), ModelID: optInt(r, "model_id"), ClassID: optInt(r, "class_id"),
		Keyword1: r.Int("keyword1"), Keyword2: r.Int("keyword2"), Env: optInt(r, "env"), Track: optInt(r, "track"), PI: optInt(r, "pi"),
		Downloads: r.Int("downloads"), Likes: r.Int("likes"), Uses: r.Int("uses"), Views: r.Int("views"), Popularity: r.Int("popularity"), Removed: r.Int("removed")}
}

func filesOf(rows []db.Row) []*File {
	out := make([]*File, len(rows))
	for i := range rows {
		out[i] = fileOf(&rows[i])
	}
	return out
}

// Versioned is one immutable livery / layer group / tune.
type Versioned struct {
	ID          int64
	Kind        string
	CarID       *int64
	Name        string
	Creator     int64
	Layers      int64
	Data        []byte
	Thumb       []byte
	ThumbSmall  []byte
	Parts       *string
	Tuning      *string
	Refs        int64
	Created     float64
	DamagedCols []string
}

// Store is the content store.
type Store struct{ DB *db.DB }

// New makes a store.
func New(d *db.DB) *Store { return &Store{d} }

// ---------------------------------------------------------------- large blobs (Content / LargeFileUpload)

// BlobCreate opens a blob of a declared total size.
func (s *Store) BlobCreate(owner int64, name string, total int64) (int64, error) {
	if total < 0 || total > MaxBlobSize {
		return 0, limitf("a blob may be at most %d bytes (declared %d)", MaxBlobSize, total)
	}
	var id int64
	err := s.DB.Do(func(t *db.Tx) error {
		q, err := t.One("SELECT COUNT(*) AS n, COALESCE(SUM(total_size), 0) AS b FROM ugc_blobs WHERE owner=?", owner)
		if err != nil {
			return err
		}
		n, b := q.Int("n"), q.Int("b")
		if n >= MaxBlobsPerOwner || b+total > MaxBytesPerOwner {
			return limitf("upload quota of this account reached (%d of %d blobs, %s + %d of %d bytes)", n, MaxBlobsPerOwner, pyfmt.ReprFloat(float64(b)), total, MaxBytesPerOwner)
		}
		res, err := t.Exec("INSERT INTO ugc_blobs (owner, user_file_name, total_size, received, data, created) VALUES (?,?,?,0,?,?)",
			owner, name, total, []byte{}, float64(time.Now().UnixNano())/1e9)
		id = res.LastID
		return err
	})
	return id, err
}

// BlobWrite writes a chunk at offset into a blob of owner and returns the number of bytes received so far (ok=false for an unknown id or somebody
// else's blob). finish converts the completed blob before it counts as complete.
func (s *Store) BlobWrite(blobID, offset int64, data []byte, owner int64, finish func([]byte) []byte) (int64, bool, error) {
	if offset < 0 || len(data) > MaxChunk {
		return 0, false, limitf("bad upload chunk (offset %d, %d bytes, limit %d per chunk) for blob %d", offset, len(data), MaxChunk, blobID)
	}
	var got int64
	var found bool
	err := s.DB.Do(func(t *db.Tx) error {
		r, err := t.One("SELECT data, total_size, received FROM ugc_blobs WHERE id=? AND owner=?", blobID, owner)
		if err != nil || r == nil {
			return err
		}
		total, received := r.Int("total_size"), r.Int("received")
		if finish != nil && received >= total {
			got, found = received, true // a repeated last chunk: the stored blob is already converted
			return nil
		}
		if offset+int64(len(data)) > min(total, MaxBlobSize) {
			return limitf("upload chunk beyond the declared size (blob %d: offset %d + %d bytes, declared %d)", blobID, offset, len(data), total)
		}
		cur, err := Unpack(r.Bytes("data"))
		if err != nil {
			return err
		}
		buf := append([]byte(nil), cur...)
		if offset > int64(len(buf)) {
			buf = append(buf, make([]byte, offset-int64(len(buf)))...)
		}
		if end := offset + int64(len(data)); end > int64(len(buf)) {
			buf = append(buf, make([]byte, end-int64(len(buf)))...)
		}
		copy(buf[offset:], data)
		done := int64(len(buf)) >= total
		uploaded := int64(len(buf))
		if done && finish != nil {
			buf = finish(buf)
			if int64(len(buf)) != uploaded {
				if _, err := t.Exec("UPDATE ugc_blobs SET total_size=? WHERE id=? AND owner=?", len(buf), blobID, owner); err != nil {
					return err
				}
				if _, err := t.Exec("UPDATE ugc_files SET payload_size=? WHERE blob_id=?", len(buf), blobID); err != nil {
					return err
				}
			}
		}
		stored := PackStored(buf)
		if done {
			stored = Pack(buf)
		}
		res, err := t.Exec("UPDATE ugc_blobs SET data=?, received=? WHERE id=? AND owner=?", stored, len(buf), blobID, owner)
		if err != nil {
			return err
		}
		if res.RowsAffected > 0 {
			got, found = uploaded, true
		}
		return nil
	})
	return got, found && err == nil, err
}

// BlobChunk is a piece of a blob.
type BlobChunk struct {
	Data  []byte
	Last  bool
	Total int
}

// BlobRead reads size bytes from offset (size 0 = to the end); nil for an unknown id.
func (s *Store) BlobRead(blobID, offset, size int64) (*BlobChunk, error) {
	r, err := s.DB.One("SELECT data, total_size FROM ugc_blobs WHERE id=?", blobID)
	if err != nil || r == nil {
		return nil, err
	}
	data, err := Unpack(r.Bytes("data"))
	if err != nil {
		return nil, err
	}
	a := min(max(offset, 0), int64(len(data)))
	z := int64(len(data))
	if size != 0 {
		z = min(a+size, z)
	}
	chunk := data[a:z]
	return &BlobChunk{chunk, offset+int64(len(chunk)) >= int64(len(data)), len(data)}, nil
}

// ContentBlob is the blob behind a Content.Upload/DownloadContent id: a photo's (or replay's) storefront file id names its blob; any other id is a
// blob id from LargeFileUpload.Create.
func (s *Store) ContentBlob(contentID int64) (int64, error) {
	q := strings.TrimSuffix(strings.Repeat("?,", len(ContentKinds)), ",")
	args := []any{contentID}
	for _, k := range ContentKinds {
		args = append(args, k)
	}
	r, err := s.DB.One("SELECT blob_id FROM ugc_files WHERE id=? AND kind IN ("+q+") AND blob_id IS NOT NULL", args...)
	if err != nil {
		return 0, err
	}
	if r != nil {
		return r.Int("blob_id"), nil
	}
	return contentID, nil
}

// IsPhoto says whether the content id is a photo's storefront file.
func (s *Store) IsPhoto(contentID int64) (bool, error) {
	r, err := s.DB.One("SELECT 1 FROM ugc_files WHERE id=? AND kind='photo' AND blob_id IS NOT NULL", contentID)
	return r != nil, err
}

// BlobByName is the newest blob of an owner with that user file name (id 0 = none).
func (s *Store) BlobByName(owner int64, name string) (int64, []byte, error) {
	r, err := s.DB.One("SELECT id, data FROM ugc_blobs WHERE owner=? AND user_file_name=? ORDER BY id DESC LIMIT 1", owner, name)
	if err != nil || r == nil {
		return 0, nil, err
	}
	d, err := Unpack(r.Bytes("data"))
	return r.Int("id"), d, err
}

// ---------------------------------------------------------------- versioned items

func jsonText(v any) (any, error) {
	if v == nil {
		return nil, nil
	}
	return pyjson.Dumps(v, pyjson.Opts{})
}

// VersionedCreate stores a new versioned item and returns its id. data nil = none.
func (s *Store) VersionedCreate(kind string, owner int64, name string, carID *int64, thumb []byte, layers int64, parts, tuning any, data []byte) (int64, error) {
	if layers < 0 {
		layers = 0
	}
	var packed any
	if data != nil {
		packed = Pack(data)
	}
	p, err := jsonText(parts)
	if err != nil {
		return 0, err
	}
	tu, err := jsonText(tuning)
	if err != nil {
		return 0, err
	}
	var car any
	if carID != nil {
		car = *carID
	}
	var id int64
	err = s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("INSERT INTO ugc_versioned (kind, car_id, name, creator, layers, data, thumb, thumb_small, parts, tuning, refs, created) "+
			"VALUES (?,?,?,?,?,?,?,?,?,?,1,?)", kind, car, name, owner, layers, packed, Pack(thumb), Pack(thumb), p, tu, float64(time.Now().UnixNano())/1e9)
		id = res.LastID
		return err
	})
	return id, err
}

// VersionedGet reads a versioned item (nil = none). A damaged blob column reads as empty and is named in DamagedCols.
func (s *Store) VersionedGet(vid int64) (*Versioned, error) {
	r, err := s.DB.One("SELECT * FROM ugc_versioned WHERE id=?", vid)
	if err != nil || r == nil {
		return nil, err
	}
	v := &Versioned{ID: r.Int("id"), Kind: r.Str("kind"), CarID: optInt(r, "car_id"), Name: r.Str("name"), Creator: r.Int("creator"), Layers: r.Int("layers"),
		Refs: r.Int("refs"), Created: r.Float("created")}
	if !r.IsNull("parts") {
		x := r.Str("parts")
		v.Parts = &x
	}
	if !r.IsNull("tuning") {
		x := r.Str("tuning")
		v.Tuning = &x
	}
	for _, c := range []struct {
		k   string
		dst *[]byte
	}{{"data", &v.Data}, {"thumb", &v.Thumb}, {"thumb_small", &v.ThumbSmall}} {
		d, err := Unpack(r.Bytes(c.k))
		var le *LimitError
		if errors.As(err, &le) {
			d = []byte{}
			v.DamagedCols = append(v.DamagedCols, c.k)
			if ok, _ := rate.Allow(fmt.Sprintf("damaged/%d/%s", vid, c.k), DamagedLogSeconds); ok {
				log.Error(fmt.Sprintf("versioned item %d (%s, creator %d) has a damaged %s, served empty: %v", vid, v.Kind, v.Creator, c.k, err))
			}
		}
		*c.dst = d
	}
	return v, nil
}

// VersionedSetData replaces the data; creator != 0 restricts it to the item's creator. Returns the rows changed.
func (s *Store) VersionedSetData(vid int64, data []byte, creator int64) (int64, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		var res db.Result
		var err error
		if creator == 0 {
			res, err = t.Exec("UPDATE ugc_versioned SET data=? WHERE id=?", Pack(data), vid)
		} else {
			res, err = t.Exec("UPDATE ugc_versioned SET data=? WHERE id=? AND creator=?", Pack(data), vid, creator)
		}
		n = res.RowsAffected
		return err
	})
	return n, err
}

// VersionedSetThumbs replaces the thumbnails; creator != 0 restricts it to the item's creator.
func (s *Store) VersionedSetThumbs(vid int64, big, small []byte, creator int64) (int64, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		var res db.Result
		var err error
		if creator == 0 {
			res, err = t.Exec("UPDATE ugc_versioned SET thumb=?, thumb_small=? WHERE id=?", Pack(big), Pack(small), vid)
		} else {
			res, err = t.Exec("UPDATE ugc_versioned SET thumb=?, thumb_small=? WHERE id=? AND creator=?", Pack(big), Pack(small), vid, creator)
		}
		n = res.RowsAffected
		return err
	})
	return n, err
}

// VersionedSetLayers sets the layer count (the client sends -1 for "unknown"; a stored count is never negative).
func (s *Store) VersionedSetLayers(vid, layers, creator int64) (int64, error) {
	layers = max(layers, 0)
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("UPDATE ugc_versioned SET layers=? WHERE id=? AND creator=?", layers, vid, creator)
		n = res.RowsAffected
		return err
	})
	return n, err
}

// VersionedRef changes the reference count (not below 0).
func (s *Store) VersionedRef(vid, delta int64) error {
	return s.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("UPDATE ugc_versioned SET refs = GREATEST(0, refs + ?) WHERE id=?", delta, vid)
		return err
	})
}

// ---------------------------------------------------------------- storefront files

// FileSpec is what FileCreate takes.
type FileSpec struct {
	Kind                                    string
	Owner                                   int64
	Title, Description                      string
	Type                                    int64
	VersionedID                             *int64
	PayloadSize                             int64
	Preview                                 []byte
	BlobID, CarID, MakeID, ModelID, ClassID *int64
	Keyword1, Keyword2                      int64
	Env, Track, PI                          *int64
}

func ptr(p *int64) any {
	if p == nil {
		return nil
	}
	return *p
}

// FileCreate lists a file in the storefront and returns its id.
func (s *Store) FileCreate(f FileSpec) (int64, error) {
	var id int64
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("INSERT INTO ugc_files (kind, type, owner, title, description, created, payload_size, preview, versioned_id, blob_id, "+
			"car_id, make_id, model_id, class_id, keyword1, keyword2, env, track, pi) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
			f.Kind, f.Type, f.Owner, trunc(f.Title, 33), trunc(f.Description, 513), float64(time.Now().UnixNano())/1e9, f.PayloadSize, Pack(f.Preview),
			ptr(f.VersionedID), ptr(f.BlobID), ptr(f.CarID), ptr(f.MakeID), ptr(f.ModelID), ptr(f.ClassID), f.Keyword1, f.Keyword2, ptr(f.Env), ptr(f.Track), ptr(f.PI))
		id = res.LastID
		return err
	})
	return id, err
}

// FileGet reads a listed file with its preview (nil = none or removed).
func (s *Store) FileGet(fid int64) (*File, error) {
	r, err := s.DB.One("SELECT * FROM ugc_files WHERE id=? AND removed=0", fid)
	if err != nil || r == nil {
		return nil, err
	}
	f := fileOf(r)
	f.Preview, err = Unpack(r.Bytes("preview"))
	return f, err
}

// Files reads the listed files among ids, in order.
func (s *Store) Files(ids []int64) ([]*File, error) {
	var out []*File
	for _, i := range ids {
		f, err := s.FileGet(i)
		if err != nil {
			return nil, err
		}
		if f != nil {
			out = append(out, f)
		}
	}
	return out, nil
}

// Mine lists an owner's files of a type, newest first.
func (s *Store) Mine(owner, ftype int64, limit int) ([]*File, error) {
	rows, err := s.DB.Query("SELECT "+ListCols+" FROM ugc_files WHERE owner=? AND type=? AND removed=0 ORDER BY id DESC LIMIT ?", owner, ftype, limit)
	return filesOf(rows), err
}

// SearchSpec is the filter of Search; 0 / -1 mean "any".
type SearchSpec struct {
	Kind                         string
	Make, Model                  int64
	Class                        *int64 // nil = any
	Kw1, Kw2, Env, Track, Layers int64
	Gamertag, Text               string
	Limit                        int
}

// Search lists public files of a kind, best liked first (then newest).
func (s *Store) Search(q SearchSpec) ([]*File, error) {
	where, args := []string{"f.kind=?", "f.removed=0", Complete}, []any{q.Kind}
	for _, c := range []struct {
		col string
		val int64
	}{{"make_id", q.Make}, {"model_id", q.Model}, {"keyword1", q.Kw1}, {"keyword2", q.Kw2}, {"env", q.Env}, {"track", q.Track}} {
		if c.val > 0 {
			where = append(where, "f."+c.col+"=?")
			args = append(args, c.val)
		}
	}
	if q.Class != nil && *q.Class >= 0 {
		where = append(where, "f.class_id=?")
		args = append(args, *q.Class)
	}
	if q.Text != "" {
		where = append(where, "(f.title ILIKE ? OR f.description ILIKE ?)")
		args = append(args, "%"+q.Text+"%", "%"+q.Text+"%")
	}
	if q.Gamertag != "" {
		where = append(where, "f.owner IN (SELECT xuid FROM players WHERE lower(gamertag)=lower(?))")
		args = append(args, q.Gamertag)
	}
	rows, err := s.DB.Query("SELECT "+listColsF+" FROM ugc_files f WHERE "+strings.Join(where, " AND ")+" ORDER BY "+ScoreSQL+" DESC, f.id DESC LIMIT ?", append(args, q.Limit)...)
	return filesOf(rows), err
}

// Curated is the recommended content: files of a type for a car (any car when carID <= 0), most liked first.
func (s *Store) Curated(ctype, carID int64, limit int) ([]*File, error) {
	rows, err := s.DB.Query("SELECT "+listColsF+" FROM ugc_files f WHERE f.removed=0 AND f.type=? AND (?<=0 OR f.car_id=?) ORDER BY "+ScoreSQL+" DESC, f.id DESC LIMIT ?",
		ctype, carID, carID, limit)
	return filesOf(rows), err
}

// Popular lists complete public files of a kind, best liked first (then most viewed, newest); since > 0 keeps files shared after that time.
func (s *Store) Popular(kind string, limit int, since float64) ([]*File, error) {
	where, args := []string{"f.kind=?", "f.removed=0", Complete}, []any{kind}
	if since > 0 {
		where = append(where, "f.created >= ?")
		args = append(args, since)
	}
	rows, err := s.DB.Query("SELECT "+listColsF+" FROM ugc_files f WHERE "+strings.Join(where, " AND ")+" ORDER BY "+ScoreSQL+" DESC, f.views DESC, f.id DESC LIMIT ?", append(args, limit)...)
	return filesOf(rows), err
}

// Bump counts one view / like / use / download.
func (s *Store) Bump(fid int64, column string) error {
	switch column {
	case "views", "likes", "uses", "downloads":
	default:
		panic("ugc: bump of " + column)
	}
	return s.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec(fmt.Sprintf("UPDATE ugc_files SET %s=%s+1 WHERE id=?", column, column), fid)
		return err
	})
}

// RecordAction counts column of file fid once for this player and action ("download"/"like"); false for a repeat, the file's own creator or an
// unknown / removed file.
func (s *Store) RecordAction(fid, xuid int64, action, column string) (bool, error) {
	if (action != "download" && action != "like") || (column != "downloads" && column != "likes") {
		panic("ugc: action " + action + " / column " + column)
	}
	counted := false
	err := s.DB.Do(func(t *db.Tx) error {
		f, err := t.One("SELECT owner FROM ugc_files WHERE id=? AND removed=0", fid)
		if err != nil || f == nil || f.Int("owner") == xuid {
			return err
		}
		res, err := t.Exec("INSERT INTO ugc_actions (file_id, xuid, action, at) VALUES (?,?,?,?) ON CONFLICT DO NOTHING", fid, xuid, action, float64(time.Now().UnixNano())/1e9)
		if err != nil || res.RowsAffected == 0 {
			return err
		}
		if _, err := t.Exec(fmt.Sprintf("UPDATE ugc_files SET %s=%s+1 WHERE id=?", column, column), fid); err != nil {
			return err
		}
		counted = true
		return nil
	})
	return counted, err
}

// FileOfVersioned is the newest public file that lists a versioned item (0 = none).
func (s *Store) FileOfVersioned(vid int64) (int64, error) {
	r, err := s.DB.One("SELECT id FROM ugc_files WHERE versioned_id=? AND removed=0 ORDER BY id DESC LIMIT 1", vid)
	if err != nil || r == nil {
		return 0, err
	}
	return r.Int("id"), nil
}

// Remove takes a file off the storefront (only its owner can); returns the rows changed.
func (s *Store) Remove(fid, owner int64) (int64, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("UPDATE ugc_files SET removed=1 WHERE id=? AND owner=?", fid, owner)
		n = res.RowsAffected
		return err
	})
	return n, err
}

// Retitle changes the title and/or description of an owner's file.
func (s *Store) Retitle(fid, owner int64, title, description *string) error {
	return s.DB.Do(func(t *db.Tx) error {
		if title != nil {
			if _, err := t.Exec("UPDATE ugc_files SET title=? WHERE id=? AND owner=?", trunc(*title, 33), fid, owner); err != nil {
				return err
			}
		}
		if description != nil {
			if _, err := t.Exec("UPDATE ugc_files SET description=? WHERE id=? AND owner=?", trunc(*description, 513), fid, owner); err != nil {
				return err
			}
		}
		return nil
	})
}

// Count is the number of public files (of a kind when given).
func (s *Store) Count(kind string) (int64, error) {
	var r *db.Row
	var err error
	if kind != "" {
		r, err = s.DB.One("SELECT COUNT(*) FROM ugc_files WHERE removed=0 AND kind=?", kind)
	} else {
		r, err = s.DB.One("SELECT COUNT(*) FROM ugc_files WHERE removed=0")
	}
	if err != nil {
		return 0, err
	}
	return r.IntAt(0), nil
}

// Recompress packs every stored blob that is still raw. Returns (bytes before, bytes after) of the rows that were rewritten.
func (s *Store) Recompress() (before, after int64, err error) {
	err = s.DB.Do(func(t *db.Tx) error {
		for _, tc := range []struct {
			table string
			cols  []string
		}{{"ugc_blobs", []string{"data"}}, {"ugc_versioned", []string{"data", "thumb", "thumb_small"}}, {"ugc_files", []string{"preview"}}} {
			rows, err := t.Query(fmt.Sprintf("SELECT id, %s FROM %s", strings.Join(tc.cols, ", "), tc.table))
			if err != nil {
				return err
			}
			for i := range rows {
				r := &rows[i]
				for _, col := range tc.cols {
					v := r.Bytes(col)
					if len(v) == 0 {
						continue
					}
					if len(v) >= 3 && bytes.Equal(v[:3], Magic) {
						continue
					}
					nv := Pack(v)
					if _, err := t.Exec(fmt.Sprintf("UPDATE %s SET %s=? WHERE id=?", tc.table, col), nv, r.Int("id")); err != nil {
						return err
					}
					before += int64(len(v))
					after += int64(len(nv))
				}
			}
		}
		return nil
	})
	return
}

// RepairLiveries drops the stale 13-byte prefix from livery rows stored by the first parser. Returns the number of rows fixed.
func (s *Store) RepairLiveries() (int, error) {
	n := 0
	err := s.DB.Do(func(t *db.Tx) error {
		rows, err := t.Query("SELECT id, data FROM ugc_versioned WHERE kind='livery' AND data IS NOT NULL")
		if err != nil {
			return err
		}
		for i := range rows {
			old, err := Unpack(rows[i].Bytes("data"))
			if err != nil {
				return err
			}
			if nw := StripLiveryPrefix(old); !bytes.Equal(nw, old) {
				if _, err := t.Exec("UPDATE ugc_versioned SET data=? WHERE id=?", Pack(nw), rows[i].Int("id")); err != nil {
					return err
				}
				n++
			}
		}
		return nil
	})
	return n, err
}

// RepairPhotos converts completed photo uploads that are not 1920x1080 yet. Returns the number of photos rewritten.
func (s *Store) RepairPhotos() (int, error) {
	rows, err := s.DB.Query("SELECT f.id AS fid, b.id AS bid, b.data FROM ugc_files f JOIN ugc_blobs b ON b.id = f.blob_id " +
		"WHERE f.kind='photo' AND b.received >= b.total_size AND b.total_size > 0")
	if err != nil {
		return 0, err
	}
	n := 0
	for i := range rows {
		old, err := Unpack(rows[i].Bytes("data"))
		if err != nil {
			return n, err
		}
		nw := Photo1080p(old)
		if bytes.Equal(nw, old) {
			continue
		}
		err = s.DB.Do(func(t *db.Tx) error {
			if _, err := t.Exec("UPDATE ugc_blobs SET data=?, total_size=?, received=? WHERE id=?", Pack(nw), len(nw), len(nw), rows[i].Int("bid")); err != nil {
				return err
			}
			_, err := t.Exec("UPDATE ugc_files SET payload_size=? WHERE id=?", len(nw), rows[i].Int("fid"))
			return err
		})
		if err != nil {
			return n, err
		}
		n++
	}
	return n, nil
}

// StoredBytes is the size of everything stored.
func (s *Store) StoredBytes() (int64, error) {
	r, err := s.DB.One("SELECT (SELECT COALESCE(SUM(LENGTH(data)),0) FROM ugc_blobs) + (SELECT COALESCE(SUM(LENGTH(data)+LENGTH(thumb)+LENGTH(thumb_small)),0) " +
		"FROM ugc_versioned) + (SELECT COALESCE(SUM(LENGTH(preview)),0) FROM ugc_files)")
	if err != nil {
		return 0, err
	}
	return r.IntAt(0), nil
}
