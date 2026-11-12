package main

import (
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/gif"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strconv"
	"strings"
)

// 以下结构与固件 app/library.cc 解析的 library.json 一一对应。
type libraryOut struct {
	VolumeSound string     `json:"volumeSound"`
	Font16      string     `json:"font16"`
	Font12      string     `json:"font12"`
	Groups      []groupOut `json:"groups"`
}

type groupOut struct {
	Name       string     `json:"name"`       // 列表行、状态栏标题
	TitleAudio string     `json:"titleAudio"` // 组名朗读
	Entries    []entryOut `json:"entries"`
}

// 组里的一项：type 为 story（故事、古诗）或 figure（车辆、动物：原图 + 介绍 + 动画）。
type entryOut struct {
	Type       string     `json:"type"`
	Name       string     `json:"name"`
	TitleAudio string     `json:"titleAudio,omitempty"` // story：标题朗读
	Pages      []pageOut  `json:"pages,omitempty"`
	Base       string     `json:"base,omitempty"`
	Intro      string     `json:"intro,omitempty"`
	Frames     []frameOut `json:"frames,omitempty"` // figure：动画关键帧
}

type pageOut struct {
	Img   string `json:"img"`
	Audio string `json:"audio"`
}

type frameOut struct {
	Img string `json:"img"`
	Ms  int    `json:"ms"`
}

// 顶部 24px 是固件绘制的状态栏；画面里不放文字（小孩不识字），图片等比缩放进状态栏下方的区域。
const statusBarHeight = 24

var artBox = image.Rect(8, statusBarHeight+6, 392, 294)

// 状态栏 12px 字库只用于数字（进度、电量）。16px 字库收录全部可打印 ASCII、各组和各项的标题，
// 以及固件写死的中文：app.cc 的「目录」「设置」「音量」和设置页信息行、ui/menu.h 的「返回」。固件新增中文时同步这里。
const (
	digitChars     = "0123456789/%-"
	firmwareLabels = "返回目录设置音量电池充电中已充满存储已用版本"
)

func printableASCII() string {
	var b strings.Builder
	for r := rune(0x20); r < 0x7F; r++ {
		b.WriteRune(r)
	}
	return b.String()
}

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
	data, err := os.ReadFile(src)
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
	out := libraryOut{VolumeSound: "ui/volume.ogg", Font16: "ui/font16.bin", Font12: "ui/font12.bin"}
	beep := filepath.Join(b.tts.cacheDir, "volume.ogg")
	b.tts.keep(beep)
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
	captions := printableASCII() + firmwareLabels
	for gi, g := range lib.Groups {
		o := groupOut{Name: g.Title, TitleAudio: fmt.Sprintf("groups/%d.ogg", gi+1)}
		if err := b.writeSpeech(o.TitleAudio, g.Title); err != nil {
			return err
		}
		for _, name := range g.Items {
			entries, err := b.buildItem(name)
			if err != nil {
				return fmt.Errorf("%s: %w", name, err)
			}
			o.Entries = append(o.Entries, entries...)
		}
		if len(o.Entries) == 0 {
			return fmt.Errorf("组 %s 为空", g.Title)
		}
		captions += o.Name
		for _, e := range o.Entries {
			captions += e.Name
		}
		out.Groups = append(out.Groups, o)
	}
	if len(out.Groups) == 0 {
		return fmt.Errorf("library.yaml 没有组")
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

// buildItem 编译 content/<name>：故事是一项，形象合集（车辆、动物）展开成每个形象一项。
func (b *builder) buildItem(name string) ([]entryOut, error) {
	it, err := loadItem(b.content, name)
	if err != nil {
		return nil, err
	}
	dir := filepath.Join(b.content, name)
	if it.Type == "figures" {
		var out []entryOut
		for _, f := range it.Figures {
			e, err := b.buildFigure(name, dir, f)
			if err != nil {
				return nil, fmt.Errorf("%s: %w", f.ID, err)
			}
			out = append(out, *e)
		}
		return out, nil
	}
	e := entryOut{Type: "story", Name: it.Title, TitleAudio: name + "/title.ogg"}
	spoken := it.Title
	if it.Author != "" {
		spoken += "，" + it.Author
	}
	if err := b.writeSpeech(e.TitleAudio, spoken); err != nil {
		return nil, err
	}
	for i, p := range it.Pages {
		po, err := b.buildPage(name, dir, i, p)
		if err != nil {
			return nil, fmt.Errorf("第 %d 页: %w", i+1, err)
		}
		e.Pages = append(e.Pages, *po)
	}
	return []entryOut{e}, nil
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

func (b *builder) buildFigure(name, dir string, fig figureSrc) (*entryOut, error) {
	fdir := filepath.Join(dir, fig.ID)
	prefix := path.Join(name, fig.ID)
	base, err := loadGray(filepath.Join(fdir, "base.png"))
	if err != nil {
		return nil, err
	}
	// 先合成所有关键帧：裁剪区域取 base 和全部关键帧内容的并集，伸出原图的部分（吊起的箱子、水柱）不会被截掉。
	composed := map[string]*image.Gray{}
	crop := trimBox(base)
	for _, f := range fig.Frames {
		if f.Image == "base.png" {
			continue
		}
		key := fmt.Sprint(f.Image, f.Region)
		if _, ok := composed[key]; ok {
			continue
		}
		g, err := loadGray(filepath.Join(fdir, f.Image))
		if err != nil {
			return nil, err
		}
		if g, err = composeRegion(base, g, f.Region); err != nil {
			return nil, fmt.Errorf("%s: %w", f.Image, err)
		}
		composed[key] = g
		crop = crop.Union(trimBox(g))
	}
	place := fitPlacement(crop, artBox)
	render := func(g *image.Gray) *frame {
		c := newCanvas()
		c.drawImage(g, place)
		return c.frame()
	}
	baseFrame := render(base)
	out := &entryOut{Type: "figure", Name: fig.Name, Base: prefix + "/base.img", Intro: prefix + "/intro.ogg"}
	if err := b.writeFrame(out.Base, baseFrame); err != nil {
		return nil, err
	}
	if err := b.writeSpeech(out.Intro, fig.Intro); err != nil {
		return nil, err
	}

	prev := baseFrame
	seq := []*frame{baseFrame}
	durations := []int{800}
	var stats []string
	written := map[string]string{} // 同一张图 + 同一区域只输出一次
	for fi, f := range fig.Frames {
		fr := baseFrame
		rel := out.Base
		if f.Image != "base.png" {
			key := fmt.Sprint(f.Image, f.Region)
			fr = render(composed[key])
			if r, ok := written[key]; ok {
				rel = r
			} else {
				rel = fmt.Sprintf("%s/f%d.img", prefix, fi+1)
				if err := b.writeFrame(rel, fr); err != nil {
					return nil, err
				}
				written[key] = rel
			}
		}
		stats = append(stats, fmt.Sprintf("%.1f%%", diffRatio(prev, fr)*100))
		out.Frames = append(out.Frames, frameOut{Img: rel, Ms: f.Ms})
		seq = append(seq, fr)
		durations = append(durations, previewRefreshMs+f.Ms)
		prev = fr
	}
	if len(fig.Frames) > 0 {
		fmt.Printf("  %s/%s: %d frames, diff %s\n", name, fig.ID, len(fig.Frames), strings.Join(stats, " "))
		if err := writeGIF(filepath.Join(b.preview, name, fig.ID, "anim.gif"), seq, durations); err != nil {
			return nil, err
		}
	}
	return out, nil
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

const nameMax = 64 // 与 esp_littlefs 的 CONFIG_LITTLEFS_OBJ_NAME_LEN 一致

// storageSize 从 firmware/partitions.csv 读 storage 分区大小（十进制、0x 十六进制，或带 K / M 后缀）。
func storageSize(partitions string) (uint64, error) {
	raw, err := os.ReadFile(partitions)
	if err != nil {
		return 0, err
	}
	for _, line := range strings.Split(string(raw), "\n") {
		cols := strings.Split(line, ",")
		if len(cols) < 5 || strings.TrimSpace(cols[0]) != "storage" {
			continue
		}
		v, mul := strings.TrimSpace(cols[4]), uint64(1)
		switch {
		case strings.HasSuffix(v, "K"):
			v, mul = strings.TrimSuffix(v, "K"), 1<<10
		case strings.HasSuffix(v, "M"):
			v, mul = strings.TrimSuffix(v, "M"), 1<<20
		}
		n, err := strconv.ParseUint(v, 0, 64)
		if err != nil {
			return 0, fmt.Errorf("%s: storage 大小 %q: %w", partitions, cols[4], err)
		}
		return n * mul, nil
	}
	return 0, fmt.Errorf("%s: 找不到 storage 分区", partitions)
}

// littlefsPython 是镜像打包工具的固定版本，由 uv 按需拉取到缓存，不需要项目内的 Python 环境。
const littlefsPython = "littlefs-python==0.19.0"

func buildImage(buildDir, fsDir, partitions string) error {
	size, err := storageSize(partitions)
	if err != nil {
		return err
	}
	out := filepath.Join(buildDir, "storage.bin")
	// --compact --no-pad：只写有数据的块，烧录更快；LittleFS 写入前会自己擦除块，不依赖其余区域为空。
	cmd := exec.Command("uvx", "--from", littlefsPython, "littlefs-python", "create", fsDir, out, "--block-size", "4096", "--fs-size", fmt.Sprint(size),
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
