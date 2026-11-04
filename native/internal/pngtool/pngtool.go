// Package pngtool is the minimal PNG decoder, encoder and area-averaging downscaler the server needs (= server/pngtool.py): the small design
// thumbnail the client expects is made from the big preview. It supports what the game writes: 8-bit RGBA / RGB / grey+alpha / grey,
// non-interlaced. Pixels are exact; the compressed bytes are not the same as Python's zlib output (Go's deflate differs), so
// tests compare decoded pixels.
package pngtool

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
)

var sig = []byte("\x89PNG\r\n\x1a\n")

var channels = map[byte]int{0: 1, 2: 3, 4: 2, 6: 4}

// MaxDim is the default size limit of Decode: the biggest picture the server handles is the 816x459 design preview.
const MaxDim = 1024

// Image is a decoded picture: Rows[y] has W*Ch bytes.
type Image struct {
	W, H, Ch int
	Rows     [][]byte
}

// Decode reads a PNG. The header is the first chunk and the only one, and its dimensions are checked before anything is allocated.
func Decode(png []byte, maxDim int) (*Image, error) {
	if len(png) < 8 || !bytes.Equal(png[:8], sig) {
		return nil, errors.New("not a PNG")
	}
	var idat [][]byte
	w, h, ctype, have := 0, 0, byte(0), false
	p := 8
	for i := 0; p+12 <= len(png); i++ {
		n := int(binary.BigEndian.Uint32(png[p:]))
		t := string(png[p+4 : p+8])
		if n < 0 || p+12+n > len(png) {
			return nil, errors.New("truncated PNG chunk")
		}
		d := png[p+8 : p+8+n]
		p += 12 + n
		switch t {
		case "IHDR":
			if i != 0 || have || len(d) != 13 {
				return nil, errors.New("bad or repeated PNG header")
			}
			w, h = int(binary.BigEndian.Uint32(d)), int(binary.BigEndian.Uint32(d[4:]))
			depth, ct, interlace := d[8], d[9], d[12]
			if _, ok := channels[ct]; depth != 8 || interlace != 0 || !ok {
				return nil, fmt.Errorf("unsupported PNG (depth %d, colour type %d, interlace %d)", depth, ct, interlace)
			}
			if !(1 <= w && w <= maxDim && 1 <= h && h <= maxDim) {
				return nil, fmt.Errorf("PNG dimensions %dx%d are outside 1..%d", w, h, maxDim)
			}
			ctype, have = ct, true
		case "IDAT":
			if !have {
				return nil, errors.New("no IHDR")
			}
			idat = append(idat, d)
		}
	}
	if !have {
		return nil, errors.New("no IHDR")
	}
	ch := channels[ctype]
	stride := w * ch
	// bounded inflate: the data of a w x h image is exactly this long, so a small file cannot expand into gigabytes
	want := h * (stride + 1)
	zr, err := zlib.NewReader(bytes.NewReader(bytes.Join(idat, nil)))
	if err != nil {
		return nil, errors.New("PNG data has the wrong size")
	}
	raw := make([]byte, want)
	if _, err := io.ReadFull(zr, raw); err != nil {
		return nil, errors.New("PNG data has the wrong size")
	}
	var extra [1]byte
	if n, _ := zr.Read(extra[:]); n > 0 {
		return nil, errors.New("PNG data has the wrong size")
	}
	rows := make([][]byte, h)
	prev := make([]byte, stride)
	q := 0
	for y := 0; y < h; y++ {
		f := raw[q]
		line := append([]byte(nil), raw[q+1:q+1+stride]...)
		q += 1 + stride
		switch f {
		case 1:
			for i := ch; i < stride; i++ {
				line[i] += line[i-ch]
			}
		case 2:
			for i := 0; i < stride; i++ {
				line[i] += prev[i]
			}
		case 3:
			for i := 0; i < stride; i++ {
				a := 0
				if i >= ch {
					a = int(line[i-ch])
				}
				line[i] += byte((a + int(prev[i])) >> 1)
			}
		case 4:
			for i := 0; i < stride; i++ {
				a, c := 0, 0
				if i >= ch {
					a, c = int(line[i-ch]), int(prev[i-ch])
				}
				b := int(prev[i])
				pa, pb, pc := abs(b-c), abs(a-c), abs(a+b-2*c)
				pr := c
				if pa <= pb && pa <= pc {
					pr = a
				} else if pb <= pc {
					pr = b
				}
				line[i] += byte(pr)
			}
		}
		rows[y] = line
		prev = line
	}
	return &Image{w, h, ch, rows}, nil
}

func abs(x int) int {
	if x < 0 {
		return -x
	}
	return x
}

func chunk(out *bytes.Buffer, t string, d []byte) {
	var n [4]byte
	binary.BigEndian.PutUint32(n[:], uint32(len(d)))
	out.Write(n[:])
	out.WriteString(t)
	out.Write(d)
	binary.BigEndian.PutUint32(n[:], crc32.ChecksumIEEE(append([]byte(t), d...)))
	out.Write(n[:])
}

// Encode writes a PNG with filter 0 on every row.
func Encode(w, h, ch int, rows [][]byte) []byte {
	ctype := map[int]byte{1: 0, 2: 4, 3: 2, 4: 6}[ch]
	var raw bytes.Buffer
	for _, r := range rows {
		raw.WriteByte(0)
		raw.Write(r)
	}
	var z bytes.Buffer
	zw, _ := zlib.NewWriterLevel(&z, zlib.BestCompression)
	zw.Write(raw.Bytes())
	zw.Close()
	var out bytes.Buffer
	out.Write(sig)
	hd := make([]byte, 13)
	binary.BigEndian.PutUint32(hd, uint32(w))
	binary.BigEndian.PutUint32(hd[4:], uint32(h))
	hd[8], hd[9] = 8, ctype
	chunk(&out, "IHDR", hd)
	chunk(&out, "IDAT", z.Bytes())
	chunk(&out, "IEND", nil)
	return out.Bytes()
}

// Downscale is the area-averaged copy of png at nw x nh (alpha averaged with the colour channels, which is fine for thumbnails).
func Downscale(png []byte, nw, nh int) ([]byte, error) {
	im, err := Decode(png, MaxDim)
	if err != nil {
		return nil, err
	}
	return Encode(nw, nh, im.Ch, DownscaleRows(im, nw, nh)), nil
}

// DownscaleRows is the pixel part of Downscale.
func DownscaleRows(im *Image, nw, nh int) [][]byte {
	w, h, ch := im.W, im.H, im.Ch
	type span struct{ a, b int }
	xs := make([]span, nw)
	for x := range xs {
		a := x * w / nw
		b := (x + 1) * w / nw
		if a+1 > b {
			b = a + 1
		}
		xs[x] = span{a, b}
	}
	out := make([][]byte, nh)
	for y := 0; y < nh; y++ {
		y0 := y * h / nh
		y1 := (y + 1) * h / nh
		if y0+1 > y1 {
			y1 = y0 + 1
		}
		band := im.Rows[y0:y1]
		line := make([]byte, nw*ch)
		for x, s := range xs {
			cnt := (s.b - s.a) * len(band)
			for c := 0; c < ch; c++ {
				sum := 0
				for _, r := range band {
					for i := s.a*ch + c; i < s.b*ch; i += ch {
						sum += int(r[i])
					}
				}
				line[x*ch+c] = byte(sum / cnt)
			}
		}
		out[y] = line
	}
	return out
}
