package main

import (
	"fmt"
	"image"
	"image/color"
	"image/draw"
)

// suggestRegion 找出关键帧相对 base 明显改动的区域。
// AI 重绘会让整张图出现零星的 1–2 像素偏移，所以按网格统计：格内差异像素超过一定比例才算改动。
func suggestRegion(base, frame *image.Gray) (image.Rectangle, *image.RGBA) {
	const cell = 24
	const minRatio = 0.06
	b := base.Bounds()
	var region image.Rectangle
	vis := image.NewRGBA(b)
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			v := frame.GrayAt(x, y).Y
			vis.Set(x, y, color.RGBA{v, v, v, 255})
		}
	}
	for cy := b.Min.Y; cy < b.Max.Y; cy += cell {
		for cx := b.Min.X; cx < b.Max.X; cx += cell {
			r := image.Rect(cx, cy, cx+cell, cy+cell).Intersect(b)
			n := 0
			for y := r.Min.Y; y < r.Max.Y; y++ {
				for x := r.Min.X; x < r.Max.X; x++ {
					if (base.GrayAt(x, y).Y < 128) != (frame.GrayAt(x, y).Y < 128) {
						n++
					}
				}
			}
			if float64(n)/float64(r.Dx()*r.Dy()) >= minRatio {
				region = region.Union(r)
				draw.Draw(vis, r, &image.Uniform{color.RGBA{255, 0, 0, 60}}, image.Point{}, draw.Over)
			}
		}
	}
	if !region.Empty() {
		region = region.Inset(-cell).Intersect(b)
		for x := region.Min.X; x < region.Max.X; x++ {
			for _, y := range []int{region.Min.Y, region.Max.Y - 1} {
				vis.Set(x, y, color.RGBA{255, 0, 0, 255})
			}
		}
		for y := region.Min.Y; y < region.Max.Y; y++ {
			for _, x := range []int{region.Min.X, region.Max.X - 1} {
				vis.Set(x, y, color.RGBA{255, 0, 0, 255})
			}
		}
	}
	return region, vis
}

func runRegion(args []string) error {
	if len(args) != 3 {
		return fmt.Errorf("usage: chalk region <base.png> <frame.png> <out.png>")
	}
	base, err := loadGray(args[0])
	if err != nil {
		return err
	}
	frame, err := loadGray(args[1])
	if err != nil {
		return err
	}
	if !base.Bounds().Eq(frame.Bounds()) {
		return fmt.Errorf("尺寸不一致：%v vs %v", base.Bounds().Size(), frame.Bounds().Size())
	}
	r, vis := suggestRegion(base, frame)
	fmt.Printf("region: [%d, %d, %d, %d]\n", r.Min.X, r.Min.Y, r.Dx(), r.Dy())
	return writePNG(args[2], vis)
}
