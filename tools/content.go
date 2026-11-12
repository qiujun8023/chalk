package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"

	"gopkg.in/yaml.v3"
)

// content/library.yaml：有哪些组、组内有哪些项，按列出的顺序播放。
type librarySrc struct {
	Groups []groupSrc `yaml:"groups"`
}

type groupSrc struct {
	ID    string   `yaml:"id"` // 组名朗读存为 content/groups/<id>.ogg
	Title string   `yaml:"title"`
	Items []string `yaml:"items"` // content/ 下含 item.yaml 的目录；figures 类型的项展开成其中的每一个形象
}

// content/<item>/item.yaml。
type itemSrc struct {
	Type    string      `yaml:"type"` // story | figures
	Title   string      `yaml:"title"`
	Author  string      `yaml:"author"` // 古诗作者，标题朗读成「诗名，作者」
	Pages   []pageSrc   `yaml:"pages"`
	Figures []figureSrc `yaml:"figures"`
}

// 第 i 页（从 1 起）的插图是 p<i>.png，旁白是 p<i>.ogg。
type pageSrc struct {
	Say string   `yaml:"say"` // 旁白
	Art *artSpec `yaml:"art"` // 插图的生成描述；缺省表示插图是手工提供的
}

// 一个形象（一辆车、一只动物）：原图 + 介绍 + 一段关键帧动画。
type figureSrc struct {
	ID    string `yaml:"id"` // 子目录名
	Name  string `yaml:"name"`
	Intro string `yaml:"intro"` // 朗读存为 <id>/intro.ogg
	// 形象目录里各张图的生成描述，键是文件名：base.png 画原图，其余在 base.png 上改出关键帧。没列出的图是手工提供的。
	Art    map[string]artSpec `yaml:"art"`
	Frames []frameSrc         `yaml:"frames"`
}

// artSpec 是一张图的生成描述：只写字符串时是 prompt，参考图取缺省值。
type artSpec struct {
	// 参考图：不含「/」时相对图片所在目录，否则相对 content/。
	// 缺省时故事页取本目录的 p01.png，形象关键帧取 base.png；故事第 1 页和形象原图必须写。
	Ref    string `yaml:"ref"`
	Prompt string `yaml:"prompt"`
}

func (a *artSpec) UnmarshalYAML(n *yaml.Node) error {
	if n.Kind == yaml.ScalarNode {
		a.Prompt = n.Value
		return nil
	}
	type plain artSpec
	var p plain
	if err := n.Decode(&p); err != nil {
		return err
	}
	for i := 0; i < len(n.Content); i += 2 {
		if k := n.Content[i].Value; k != "ref" && k != "prompt" {
			return fmt.Errorf("line %d: art 未知字段 %q", n.Content[i].Line, k)
		}
	}
	*a = artSpec(p)
	return nil
}

type frameSrc struct {
	Image string `yaml:"image"`
	// 只取 image 在这个区域内的像素，其余用 base.png，保证不动的部分逐像素一致。
	// 坐标是源图像素 [x, y, w, h]；缺省取整张图。
	Region []int `yaml:"region"`
	Ms     int   `yaml:"ms"`
}

func readYAML(path string, v any) error {
	raw, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	dec := yaml.NewDecoder(bytes.NewReader(raw))
	dec.KnownFields(true)
	if err := dec.Decode(v); err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	return nil
}

func loadItem(contentDir, name string) (*itemSrc, error) {
	var it itemSrc
	if err := readYAML(filepath.Join(contentDir, name, "item.yaml"), &it); err != nil {
		return nil, err
	}
	switch it.Type {
	case "story":
		if len(it.Pages) == 0 {
			return nil, fmt.Errorf("%s: story 没有 pages", name)
		}
	case "figures":
		if len(it.Figures) == 0 {
			return nil, fmt.Errorf("%s: figures 为空", name)
		}
		seen := map[string]bool{}
		for _, f := range it.Figures {
			if f.ID == "" || seen[f.ID] {
				return nil, fmt.Errorf("%s: figure id %q 为空或重复", name, f.ID)
			}
			seen[f.ID] = true
		}
	default:
		return nil, fmt.Errorf("%s: 未知 type %q", name, it.Type)
	}
	if it.Title == "" {
		return nil, fmt.Errorf("%s: 缺少 title", name)
	}
	return &it, nil
}
