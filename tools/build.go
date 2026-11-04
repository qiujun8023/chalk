package main

import (
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/gif"
	"io"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strings"
	"time"
)

// 以下结构与固件 app/library.cc 解析的 library.json 一一对应。
type libraryOut struct {
	VolumeSound string    `json:"volumeSound"`
	Font16      string    `json:"font16"`
	Font12      string    `json:"font12"`
	Built       string    `json:"built"` // 构建时间，设置页显示
	Items       []itemOut `json:"items"`
}

type itemOut struct {
	Type     string       `json:"type"`
	ID       string       `json:"id"`
	Name     string       `json:"name"`  // 状态栏标题
	Title    string       `json:"title"` // 标题朗读
	Pages    []pageOut    `json:"pages,omitempty"`
	Vehicles []vehicleOut `json:"vehicles,omitempty"`
}

type pageOut struct {
	Img   string `json:"img"`
	Audio string `json:"audio"`
}

type vehicleOut struct {
	Name    string      `json:"name"`
	Base    string      `json:"base"`
	Intro   string      `json:"intro"`
	Actions []actionOut `json:"actions"`
}

type actionOut struct {
	Frames []frameOut `json:"frames"`
}

type frameOut struct {
	Img string `json:"img"`
	Ms  int    `json:"ms"`
}

// 顶部 24px 是固件绘制的状态栏；画面里不放文字（小孩不识字），图片等比缩放进状态栏下方的区域。
const statusBarHeight = 24

var artBox = image.Rect(8, statusBarHeight+6, 392, 294)

// 状态栏 12px 字库只用于数字（进度、电量）；16px 字库收录标题和固件里写死的界面文字（组列表、设置页）。
const (
	digitChars  = "0123456789/%-"
	fixedLabels = "音量 返回目录设置刷新屏幕立即休眠重置进度再按确认电量电池供电充电中已充满存储内容固件MBV.:" + digitChars
)

// 预览 GIF 里每次局刷按这个耗时估算，接近实机。
const previewRefreshMs = 800

type builder struct {
	content string
	fsDir   string
	preview string
	tts     *tts
	bytes   int64
	files   int
}

func (b *builder) writeFile(rel string, data []byte) error {
	p := filepath.Join(b.fsDir, filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	b.bytes += int64(len(data))
	b.files++
	return os.WriteFile(p, data, 0o644)
}

func (b *builder) writeFrame(rel string, f *frame) error {
	if err := b.writeFile(rel, f[:]); err != nil {
		return err
	}
	pv := filepath.Join(b.preview, filepath.FromSlash(strings.TrimSuffix(rel, ".img")+".png"))
	if err := os.MkdirAll(filepath.Dir(pv), 0o755); err != nil {
		return err
	}
	return writePNG(pv, f.gray())
}

func (b *builder) writeSpeech(rel, text string) error {
	src, err := b.tts.speak(text)
	if err != nil {
		return err
	}
	return b.copyFile(rel, src)
}

func (b *builder) copyFile(rel, src string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	data, err := io.ReadAll(in)
	if err != nil {
		return err
	}
	return b.writeFile(rel, data)
}

func (b *builder) build() error {
	var lib librarySrc
	if err := readYAML(filepath.Join(b.content, "library.yaml"), &lib); err != nil {
		return err
	}
	for _, d := range []string{b.fsDir, b.preview} {
		if err := os.RemoveAll(d); err != nil {
			return err
		}
	}
	out := libraryOut{VolumeSound: "ui/volume.ogg", Font16: "ui/font16.bin", Font12: "ui/font12.bin",
		Built: time.Now().Format("01-02 15:04")}
	beep := filepath.Join(b.tts.cacheDir, "volume.ogg")
	if _, err := os.Stat(beep); err != nil {
		if err := os.MkdirAll(b.tts.cacheDir, 0o755); err != nil {
			return err
		}
		if err := volumeBeep(beep); err != nil {
			return err
		}
	}
	if err := b.copyFile(out.VolumeSound, beep); err != nil {
		return err
	}
	for _, name := range lib.Items {
		it, err := loadItem(b.content, name)
		if err != nil {
			return err
		}
		o, err := b.buildItem(name, it)
		if err != nil {
			return fmt.Errorf("%s: %w", name, err)
		}
		out.Items = append(out.Items, *o)
	}
	captions := fixedLabels
	for _, it := range out.Items {
		captions += it.Name
		for _, v := range it.Vehicles {
			captions += v.Name
		}
	}
	for _, f := range []struct {
		rel, src, chars string
	}{
		{out.Font16, "fonts/zfull_16.c", captions},
		{out.Font12, "fonts/zfull_12.c", digitChars},
	} {
		data, err := buildFont(f.src, f.chars)
		if err != nil {
			return err
		}
		if err := b.writeFile(f.rel, data); err != nil {
			return err
		}
	}
	data, err := json.MarshalIndent(out, "", "  ")
	if err != nil {
		return err
	}
	if err := b.writeFile("library.json", data); err != nil {
		return err
	}
	fmt.Printf("content: %d files, %.1f KB\n", b.files, float64(b.bytes)/1024)
	return nil
}

func (b *builder) buildItem(name string, it *itemSrc) (*itemOut, error) {
	dir := filepath.Join(b.content, name)
	o := &itemOut{Type: it.Type, ID: name, Name: it.Title, Title: name + "/title.ogg"}
	if err := b.writeSpeech(o.Title, it.Title); err != nil {
		return nil, err
	}

	switch it.Type {
	case "story":
		for i, p := range it.Pages {
			po, err := b.buildPage(name, dir, i, p)
			if err != nil {
				return nil, fmt.Errorf("第 %d 页: %w", i+1, err)
			}
			o.Pages = append(o.Pages, *po)
		}
	case "vehicles":
		for _, v := range it.Vehicles {
			vo, err := b.buildVehicle(name, dir, v)
			if err != nil {
				return nil, fmt.Errorf("%s: %w", v.ID, err)
			}
			o.Vehicles = append(o.Vehicles, *vo)
		}
	}
	return o, nil
}

func (b *builder) buildPage(name, dir string, i int, p pageSrc) (*pageOut, error) {
	stem := fmt.Sprintf("%s/p%02d", name, i+1)
	g, err := loadGray(filepath.Join(dir, p.Image))
	if err != nil {
		return nil, err
	}
	c := newCanvas()
	c.drawImage(g, fitPlacement(trimBox(g), artBox))
	if err := b.writeFrame(stem+".img", c.frame()); err != nil {
		return nil, err
	}
	if err := b.writeSpeech(stem+".ogg", p.Say); err != nil {
		return nil, err
	}
	return &pageOut{Img: stem + ".img", Audio: stem + ".ogg"}, nil
}

func (b *builder) buildVehicle(name, dir string, v vehicleSrc) (*vehicleOut, error) {
	vdir := filepath.Join(dir, v.ID)
	prefix := path.Join(name, v.ID)
	base, err := loadGray(filepath.Join(vdir, "base.png"))
	if err != nil {
		return nil, err
	}
	// 先合成所有关键帧：裁剪区域取 base 和全部关键帧内容的并集，伸出车身的部分（吊起的箱子、水柱）不会被截掉。
	composed := map[string]*image.Gray{}
	crop := trimBox(base)
	for _, a := range v.Actions {
		for _, f := range a.Frames {
			if f.Image == "base.png" {
				continue
			}
			key := fmt.Sprint(f.Image, f.Region)
			if _, ok := composed[key]; ok {
				continue
			}
			g, err := loadGray(filepath.Join(vdir, f.Image))
			if err != nil {
				return nil, err
			}
			if g, err = composeRegion(base, g, f.Region); err != nil {
				return nil, fmt.Errorf("%s: %w", f.Image, err)
			}
			composed[key] = g
			crop = crop.Union(trimBox(g))
		}
	}
	place := fitPlacement(crop, artBox)
	render := func(g *image.Gray) *frame {
		c := newCanvas()
		c.drawImage(g, place)
		return c.frame()
	}
	baseFrame := render(base)
	vo := &vehicleOut{Name: v.Name, Base: prefix + "/base.img", Intro: prefix + "/intro.ogg"}
	if err := b.writeFrame(vo.Base, baseFrame); err != nil {
		return nil, err
	}
	if err := b.writeSpeech(vo.Intro, v.Intro); err != nil {
		return nil, err
	}

	for ai, a := range v.Actions {
		ao := actionOut{}
		prev := baseFrame
		seq := []*frame{baseFrame}
		durations := []int{800}
		var stats []string
		written := map[string]string{} // 同一张图 + 同一区域只输出一次
		for fi, f := range a.Frames {
			var fr *frame
			rel := vo.Base
			if f.Image == "base.png" {
				fr = baseFrame
			} else {
				key := fmt.Sprint(f.Image, f.Region)
				fr = render(composed[key])
				if r, ok := written[key]; ok {
					rel = r
				} else {
					rel = fmt.Sprintf("%s/a%df%d.img", prefix, ai+1, fi+1)
					if err := b.writeFrame(rel, fr); err != nil {
						return nil, err
					}
					written[key] = rel
				}
			}
			stats = append(stats, fmt.Sprintf("%.1f%%", diffRatio(prev, fr)*100))
			ao.Frames = append(ao.Frames, frameOut{Img: rel, Ms: f.Ms})
			seq = append(seq, fr)
			durations = append(durations, previewRefreshMs+f.Ms)
			prev = fr
		}
		fmt.Printf("  %s/%s action %d: %d frames, diff %s\n", name, v.ID, ai+1, len(a.Frames), strings.Join(stats, " "))
		if err := writeGIF(filepath.Join(b.preview, name, v.ID, fmt.Sprintf("a%d.gif", ai+1)), seq, durations); err != nil {
			return nil, err
		}
		vo.Actions = append(vo.Actions, ao)
	}
	return vo, nil
}

func writeGIF(p string, frames []*frame, durationsMs []int) error {
	pal := color.Palette{color.White, color.Black}
	g := &gif.GIF{}
	for i, f := range frames {
		img := image.NewPaletted(image.Rect(0, 0, screenW, screenH), pal)
		for y := 0; y < screenH; y++ {
			for x := 0; x < screenW; x++ {
				if f.black(x, y) {
					img.SetColorIndex(x, y, 1)
				}
			}
		}
		g.Image = append(g.Image, img)
		g.Delay = append(g.Delay, durationsMs[i]/10)
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	out, err := os.Create(p)
	if err != nil {
		return err
	}
	defer out.Close()
	return gif.EncodeAll(out, g)
}

// ── LittleFS 镜像 ──────────────────────────────────────

const (
	storageSize = 0xDF0000 // 与 firmware/partitions.csv 的 storage 分区一致
	nameMax     = 64       // 与 esp_littlefs 的 CONFIG_LITTLEFS_OBJ_NAME_LEN 一致
)

// littlefs-python 在 Python 3.15 上编译失败，用 3.14 及以下版本建独立 venv。
func littlefsTool(buildDir string) (string, error) {
	venv := filepath.Join(buildDir, "pyenv")
	tool := filepath.Join(venv, "bin", "littlefs-python")
	if _, err := os.Stat(tool); err == nil {
		return tool, nil
	}
	var py string
	for _, c := range []string{"python3.14", "python3.13", "python3.12", "python3.11"} {
		if p, err := exec.LookPath(c); err == nil {
			py = p
			break
		}
	}
	if py == "" {
		return "", fmt.Errorf("需要 Python 3.11–3.14 来安装 littlefs-python")
	}
	for _, args := range [][]string{{py, "-m", "venv", venv}, {filepath.Join(venv, "bin", "pip"), "install", "-q", "littlefs-python"}} {
		cmd := exec.Command(args[0], args[1:]...)
		cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
		if err := cmd.Run(); err != nil {
			return "", fmt.Errorf("%s: %w", strings.Join(args, " "), err)
		}
	}
	return tool, nil
}

func buildImage(buildDir, fsDir string) error {
	tool, err := littlefsTool(buildDir)
	if err != nil {
		return err
	}
	out := filepath.Join(buildDir, "storage.bin")
	// --compact --no-pad：只写有数据的块，烧录更快；LittleFS 写入前会自己擦除块，不依赖其余区域为空。
	cmd := exec.Command(tool, "create", fsDir, out, "--block-size", "4096", "--fs-size", fmt.Sprint(storageSize),
		"--name-max", fmt.Sprint(nameMax), "--compact", "--no-pad")
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("littlefs-python: %w", err)
	}
	st, err := os.Stat(out)
	if err != nil {
		return err
	}
	fmt.Printf("image: %s (%.1f KB)\n", out, float64(st.Size())/1024)
	return nil
}
