package main

import (
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/png"
	"os"

	xdraw "golang.org/x/image/draw"
)

const (
	screenW = 400
	screenH = 300
	// 源图缩小后灰度低于这个值算黑。
	blackThreshold = 128
	// 裁白边时，灰度低于这个值算内容。
	trimThreshold = 248
)

// 1bpp 帧：每行 50 字节，MSB 在左，bit=1 白。
type frame [screenW / 8 * screenH]byte

func (f *frame) set(x, y int, black bool) {
	i := y*(screenW/8) + x/8
	bit := byte(0x80 >> (x % 8))
	if black {
		f[i] &^= bit
	} else {
		f[i] |= bit
	}
}

func (f *frame) black(x, y int) bool {
	return f[y*(screenW/8)+x/8]&(0x80>>(x%8)) == 0
}

func (f *frame) gray() *image.Gray {
	g := image.NewGray(image.Rect(0, 0, screenW, screenH))
	for y := 0; y < screenH; y++ {
		for x := 0; x < screenW; x++ {
			if f.black(x, y) {
				g.SetGray(x, y, color.Gray{0})
			} else {
				g.SetGray(x, y, color.Gray{255})
			}
		}
	}
	return g
}

// diffRatio 返回两帧不同像素占全屏的比例。
func diffRatio(a, b *frame) float64 {
	n := 0
	for i := range a {
		x := a[i] ^ b[i]
		for x != 0 {
			n++
			x &= x - 1
		}
	}
	return float64(n) / float64(screenW*screenH)
}

func loadGray(path string) (*image.Gray, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	img, _, err := image.Decode(f)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	g := image.NewGray(img.Bounds())
	// 透明背景按白色处理。
	draw.Draw(g, g.Bounds(), image.White, image.Point{}, draw.Src)
	draw.Draw(g, g.Bounds(), img, img.Bounds().Min, draw.Over)
	return g, nil
}

func trimBox(g *image.Gray) image.Rectangle {
	b := g.Bounds()
	minX, minY, maxX, maxY := b.Max.X, b.Max.Y, b.Min.X, b.Min.Y
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			if g.GrayAt(x, y).Y < trimThreshold {
				minX, minY = min(minX, x), min(minY, y)
				maxX, maxY = max(maxX, x+1), max(maxY, y+1)
			}
		}
	}
	if minX >= maxX {
		return b
	}
	return image.Rect(minX, minY, maxX, maxY)
}

// region 锁定：base 上只换 region 内的像素。
func composeRegion(base, frame *image.Gray, region []int) (*image.Gray, error) {
	if !frame.Bounds().Eq(base.Bounds()) {
		return nil, fmt.Errorf("关键帧尺寸 %v 与 base %v 不一致", frame.Bounds().Size(), base.Bounds().Size())
	}
	if len(region) == 0 {
		return frame, nil
	}
	if len(region) != 4 || region[2] <= 0 || region[3] <= 0 {
		return nil, fmt.Errorf("region %v 需要 [x, y, w, h]，宽高为正", region)
	}
	r := image.Rect(region[0], region[1], region[0]+region[2], region[1]+region[3])
	if !r.In(base.Bounds()) {
		return nil, fmt.Errorf("region %v 超出图片 %v", region, base.Bounds().Size())
	}
	out := image.NewGray(base.Bounds())
	copy(out.Pix, base.Pix)
	draw.Draw(out, r, frame, r.Min, draw.Src)
	return out, nil
}

// placement 描述源图的裁剪区域和它在屏幕上的位置。同一形象的所有关键帧共用一个 placement。
type placement struct {
	crop image.Rectangle
	dst  image.Rectangle
}

// fitPlacement 把 crop 等比缩放进 box 并居中。
func fitPlacement(crop, box image.Rectangle) placement {
	cw, ch := float64(crop.Dx()), float64(crop.Dy())
	s := min(float64(box.Dx())/cw, float64(box.Dy())/ch)
	w, h := int(cw*s+0.5), int(ch*s+0.5)
	x := box.Min.X + (box.Dx()-w)/2
	y := box.Min.Y + (box.Dy()-h)/2
	return placement{crop: crop, dst: image.Rect(x, y, x+w, y+h)}
}

type canvas struct{ g *image.Gray }

func newCanvas() *canvas {
	g := image.NewGray(image.Rect(0, 0, screenW, screenH))
	draw.Draw(g, g.Bounds(), image.White, image.Point{}, draw.Src)
	return &canvas{g}
}

func (c *canvas) drawImage(src *image.Gray, p placement) {
	// 只把黑色叠上去，多个图层可以叠加。
	tmp := image.NewGray(p.dst)
	xdraw.CatmullRom.Scale(tmp, p.dst, src, p.crop, xdraw.Src, nil)
	for y := p.dst.Min.Y; y < p.dst.Max.Y; y++ {
		for x := p.dst.Min.X; x < p.dst.Max.X; x++ {
			if v := tmp.GrayAt(x, y).Y; v < c.g.GrayAt(x, y).Y {
				c.g.SetGray(x, y, color.Gray{v})
			}
		}
	}
}

func (c *canvas) frame() *frame {
	var f frame
	for y := 0; y < screenH; y++ {
		for x := 0; x < screenW; x++ {
			f.set(x, y, c.g.GrayAt(x, y).Y < blackThreshold)
		}
	}
	return &f
}

func writePNG(path string, img image.Image) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}
