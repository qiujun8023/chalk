package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// 状态栏字库直接取自 slate 固件用的 LVGL 字体（tools/fonts/*.c，lv_font_conv 生成，1bpp、未压缩）：
// Zfull 16px 标题、Zfull 12px 数字。只收录用到的字符，转成固件 ui/font.cc 读的格式（小端）：
//
//	"CFN1" | u8 行高 | u8 基线 | u16 字数 | 字数 × {u32 码点, u8 步进, u8 宽, u8 高, i8 x, i8 y(距行顶), u32 偏移}
//	| 点阵：每行 ceil(宽/8) 字节，MSB 在左，1 = 黑
type lvGlyph struct {
	bitmapIndex, advW, boxW, boxH, ofsX, ofsY int
}

type lvCmap struct {
	start, length, gidStart int
	kind                    string
	unicodeList, ofsList    []int
}

type lvFont struct {
	bitmap             []byte
	glyphs             []lvGlyph
	cmaps              []lvCmap
	lineHeight, baseLn int
}

var (
	reHex   = regexp.MustCompile(`0x([0-9a-fA-F]+)`)
	reGlyph = regexp.MustCompile(`\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)`)
	reCmap  = regexp.MustCompile(`\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*\.glyph_id_start\s*=\s*(\d+),\s*\.unicode_list\s*=\s*(\w+),\s*\.glyph_id_ofs_list\s*=\s*(\w+),\s*\.list_length\s*=\s*(\d+),\s*\.type\s*=\s*(\w+)`)
	reNum   = regexp.MustCompile(`0x[0-9a-fA-F]+|\d+`)
)

func atoi(s string) int {
	n, _ := strconv.ParseInt(s, 0, 64)
	return int(n)
}

// block 取出 "name[] = {" 到匹配的 "};" 之间的文本。
func block(src, name string) (string, error) {
	i := strings.Index(src, name+"[] = {")
	if i < 0 {
		i = strings.Index(src, name+"[] =\n{")
	}
	if i < 0 {
		return "", fmt.Errorf("找不到 %s", name)
	}
	j := strings.Index(src[i:], "};")
	if j < 0 {
		return "", fmt.Errorf("%s 没有结尾的 };", name)
	}
	return src[i+len(name) : i+j], nil
}

func parseLVFont(path string) (*lvFont, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	src := string(raw)
	f := &lvFont{}
	b, err := block(src, "glyph_bitmap")
	if err != nil {
		return nil, err
	}
	for _, m := range reHex.FindAllStringSubmatch(b, -1) {
		v, _ := strconv.ParseUint(m[1], 16, 8)
		f.bitmap = append(f.bitmap, byte(v))
	}
	if b, err = block(src, "glyph_dsc"); err != nil {
		return nil, err
	}
	f.glyphs = append(f.glyphs, lvGlyph{}) // id 0 保留；reGlyph 也会匹配到它，下面跳过第一条
	for i, m := range reGlyph.FindAllStringSubmatch(b, -1) {
		if i == 0 {
			continue
		}
		f.glyphs = append(f.glyphs, lvGlyph{atoi(m[1]), atoi(m[2]), atoi(m[3]), atoi(m[4]), atoi(m[5]), atoi(m[6])})
	}
	if b, err = block(src, "cmaps"); err != nil {
		return nil, err
	}
	list := func(name string) ([]int, error) {
		if name == "NULL" {
			return nil, nil
		}
		body, err := block(src, name)
		if err != nil {
			return nil, err
		}
		var out []int
		for _, n := range reNum.FindAllString(body[strings.Index(body, "{")+1:], -1) {
			out = append(out, atoi(n))
		}
		return out, nil
	}
	for _, m := range reCmap.FindAllStringSubmatch(b, -1) {
		c := lvCmap{start: atoi(m[1]), length: atoi(m[2]), gidStart: atoi(m[3]), kind: m[7]}
		if c.unicodeList, err = list(m[4]); err != nil {
			return nil, err
		}
		if c.ofsList, err = list(m[5]); err != nil {
			return nil, err
		}
		f.cmaps = append(f.cmaps, c)
	}
	mh := regexp.MustCompile(`\.line_height\s*=\s*(\d+)`).FindStringSubmatch(src)
	mb := regexp.MustCompile(`\.base_line\s*=\s*(\d+)`).FindStringSubmatch(src)
	if mh == nil || mb == nil {
		return nil, fmt.Errorf("%s: 缺少 line_height/base_line", path)
	}
	f.lineHeight, f.baseLn = atoi(mh[1]), atoi(mb[1])
	return f, nil
}

// glyphID 按 LVGL 的 cmap 规则查码点。
func (f *lvFont) glyphID(cp int) int {
	for _, c := range f.cmaps {
		rcp := cp - c.start
		if rcp < 0 || rcp >= c.length {
			continue
		}
		switch c.kind {
		case "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY":
			return c.gidStart + rcp
		case "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL":
			if rcp < len(c.ofsList) && (c.ofsList[rcp] != 0 || rcp == 0) {
				return c.gidStart + c.ofsList[rcp]
			}
		case "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY":
			for i, u := range c.unicodeList {
				if u == rcp {
					return c.gidStart + i
				}
			}
		case "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL":
			for i, u := range c.unicodeList {
				if u == rcp {
					return c.gidStart + c.ofsList[i]
				}
			}
		}
	}
	return 0
}

func buildFont(path, chars string) ([]byte, error) {
	f, err := parseLVFont(path)
	if err != nil {
		return nil, err
	}
	set := map[rune]bool{}
	for _, r := range chars {
		set[r] = true
	}
	runes := make([]rune, 0, len(set))
	for r := range set {
		runes = append(runes, r)
	}
	sort.Slice(runes, func(i, j int) bool { return runes[i] < runes[j] })

	var index, bits bytes.Buffer
	for _, r := range runes {
		gid := f.glyphID(int(r))
		if gid <= 0 || gid >= len(f.glyphs) {
			return nil, fmt.Errorf("%s 缺少字符 %q", path, r)
		}
		g := f.glyphs[gid]
		off := bits.Len()
		// LVGL 1bpp 点阵按位连续存放，不按行对齐；这里拆成按行对齐。
		for y := 0; y < g.boxH; y++ {
			row := make([]byte, (g.boxW+7)/8)
			for x := 0; x < g.boxW; x++ {
				bit := y*g.boxW + x
				if f.bitmap[g.bitmapIndex+bit/8]&(0x80>>(bit%8)) != 0 {
					row[x/8] |= 0x80 >> (x % 8)
				}
			}
			bits.Write(row)
		}
		top := f.lineHeight - f.baseLn - g.ofsY - g.boxH
		binary.Write(&index, binary.LittleEndian, uint32(r))
		index.Write([]byte{byte((g.advW + 8) / 16), byte(g.boxW), byte(g.boxH), byte(int8(g.ofsX)), byte(int8(top))})
		binary.Write(&index, binary.LittleEndian, uint32(off))
	}
	var out bytes.Buffer
	out.WriteString("CFN1")
	out.Write([]byte{byte(f.lineHeight), byte(f.lineHeight - f.baseLn)})
	binary.Write(&out, binary.LittleEndian, uint16(len(runes)))
	out.Write(index.Bytes())
	out.Write(bits.Bytes())
	return out.Bytes(), nil
}
