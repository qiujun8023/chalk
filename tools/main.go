// chalk 把 content/ 下的源内容（线稿、文案）编译成设备可直接读取的内容镜像。
//
//	go run . build                          生成 build/fs、build/storage.bin 和 build/preview
//	go run . region <base> <frame> <out>    建议关键帧的 region，并输出标注图
package main

import (
	"fmt"
	"os"
	"path/filepath"
)

func main() {
	if len(os.Args) >= 2 && os.Args[1] == "region" {
		if err := runRegion(os.Args[2:]); err != nil {
			fatal(err)
		}
		return
	}
	if len(os.Args) < 2 || os.Args[1] != "build" {
		fmt.Fprintln(os.Stderr, "usage: chalk build | chalk region <base.png> <frame.png> <out.png>")
		os.Exit(2)
	}
	root, err := repoRoot()
	if err != nil {
		fatal(err)
	}
	buildDir := filepath.Join(root, "build")
	b := &builder{
		content: filepath.Join(root, "content"),
		fsDir:   filepath.Join(buildDir, "fs"),
		preview: filepath.Join(buildDir, "preview"),
		tts:     newTTS(filepath.Join(root, "content", ".cache", "tts")),
	}
	if err := b.build(); err != nil {
		fatal(err)
	}
	if err := buildImage(buildDir, b.fsDir, filepath.Join(root, "firmware", "partitions.csv")); err != nil {
		fatal(err)
	}
	if n, err := b.tts.prune(); err != nil {
		fatal(err)
	} else if n > 0 {
		fmt.Printf("tts cache: removed %d unused files\n", n)
	}
}

// repoRoot 从当前目录向上找含 content/library.yaml 的目录。
func repoRoot() (string, error) {
	dir, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "content", "library.yaml")); err == nil {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", fmt.Errorf("找不到 content/library.yaml，请在仓库内运行")
		}
		dir = parent
	}
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "chalk:", err)
	os.Exit(1)
}
