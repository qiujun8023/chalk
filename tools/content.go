package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"

	"gopkg.in/yaml.v3"
)

// content/library.yaml：组的顺序。
type librarySrc struct {
	Items []string `yaml:"items"`
}

// content/<item>/item.yaml。
type itemSrc struct {
	Type     string       `yaml:"type"` // story | vehicles
	Title    string       `yaml:"title"`
	Pages    []pageSrc    `yaml:"pages"`
	Vehicles []vehicleSrc `yaml:"vehicles"`
}

type pageSrc struct {
	Image string `yaml:"image"`
	Say   string `yaml:"say"` // 旁白
}

type vehicleSrc struct {
	ID      string      `yaml:"id"` // 子目录名
	Name    string      `yaml:"name"`
	Intro   string      `yaml:"intro"`
	Actions []actionSrc `yaml:"actions"`
}

type actionSrc struct {
	Frames []frameSrc `yaml:"frames"`
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
	case "vehicles":
		if len(it.Vehicles) == 0 {
			return nil, fmt.Errorf("%s: vehicles 为空", name)
		}
	default:
		return nil, fmt.Errorf("%s: 未知 type %q", name, it.Type)
	}
	if it.Title == "" {
		return nil, fmt.Errorf("%s: 缺少 title", name)
	}
	return &it, nil
}
