package main

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
)

// asset 是一个由 AI 生成、入库的文件：朗读音频或插图。
// 每个内容目录的 gen.lock 记录其中各文件生成时的输入 hash，输入变了即过期，需要 chalk gen 重新生成。
type asset struct {
	rel  string // 相对 content/
	lock string // gen.lock 所在目录，相对 content/
	text string // 音频：朗读文本
	art  *artSpec
	kind string // 图片的提示词模板：page | figure | frame
	ref  string // 图片的参考图，相对 content/
}

func (a *asset) speech() bool { return a.art == nil }

// 同目录的参考图（故事第 1 页、形象原图）是依赖：按内容计入 hash，重画后引用它的图随之过期。
// 跨目录的参考图只借画风，按路径计入。
func (a *asset) refIsDep() bool { return path.Dir(a.ref) == path.Dir(a.rel) }

// collectAssets 按 library.yaml 列出全部生成文件。
func collectAssets(content string, lib *librarySrc) ([]*asset, error) {
	var out []*asset
	seen := map[string]bool{}
	add := func(a *asset) error {
		if seen[a.rel] {
			return fmt.Errorf("%s 重复", a.rel)
		}
		seen[a.rel] = true
		if a.speech() {
			if a.text = strings.TrimSpace(a.text); a.text == "" {
				return fmt.Errorf("%s: 朗读文本为空", a.rel)
			}
		} else if strings.TrimSpace(a.art.Prompt) == "" {
			return fmt.Errorf("%s: art 缺少 prompt", a.rel)
		}
		out = append(out, a)
		return nil
	}
	image := func(rel, lock, kind string, art artSpec, defRef string) error {
		ref := art.Ref
		if ref == "" {
			ref = defRef
		}
		if ref == "" {
			return fmt.Errorf("%s: art 缺少 ref", rel)
		}
		if strings.Contains(ref, "/") {
			ref = path.Clean(ref)
		} else {
			ref = path.Join(path.Dir(rel), ref)
		}
		if ref == rel {
			return fmt.Errorf("%s: 参考图是自己", rel)
		}
		return add(&asset{rel: rel, lock: lock, art: &art, kind: kind, ref: ref})
	}
	for _, g := range lib.Groups {
		if g.ID == "" || strings.ContainsAny(g.ID, "/.") {
			return nil, fmt.Errorf("组 %s: id %q 无效", g.Title, g.ID)
		}
		if err := add(&asset{rel: "groups/" + g.ID + ".ogg", lock: "groups", text: g.Title}); err != nil {
			return nil, err
		}
		for _, name := range g.Items {
			it, err := loadItem(content, name)
			if err != nil {
				return nil, err
			}
			if it.Type == "figures" {
				for _, f := range it.Figures {
					dir := path.Join(name, f.ID)
					if err := add(&asset{rel: dir + "/intro.ogg", lock: name, text: f.Intro}); err != nil {
						return nil, err
					}
					files := make([]string, 0, len(f.Art))
					for k := range f.Art {
						files = append(files, k)
					}
					sort.Strings(files)
					for _, k := range files {
						kind, def := "frame", "base.png"
						if k == "base.png" {
							kind, def = "figure", ""
						}
						if err := image(dir+"/"+k, name, kind, f.Art[k], def); err != nil {
							return nil, err
						}
					}
				}
				continue
			}
			if err := add(&asset{rel: name + "/title.ogg", lock: name, text: spokenTitle(it)}); err != nil {
				return nil, err
			}
			for i, p := range it.Pages {
				stem := fmt.Sprintf("%s/p%02d", name, i+1)
				if err := add(&asset{rel: stem + ".ogg", lock: name, text: p.Say}); err != nil {
					return nil, err
				}
				if p.Art != nil {
					def := "p01.png"
					if i == 0 {
						def = ""
					}
					if err := image(stem+".png", name, "page", *p.Art, def); err != nil {
						return nil, err
					}
				}
			}
		}
	}
	return out, nil
}

func spokenTitle(it *itemSrc) string {
	if it.Author != "" {
		return it.Title + "，" + it.Author
	}
	return it.Title
}

func fileHash(p string) (string, error) {
	data, err := os.ReadFile(p)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:]), nil
}

// inputHash 是 a 当前输入的 hash，与 gen.lock 里的记录比较判断是否过期。
func inputHash(content string, a *asset) (string, error) {
	if a.speech() {
		return hashOf("speech", ttsProvider(), a.text), nil
	}
	ref := "path:" + a.ref
	if a.refIsDep() {
		h, err := fileHash(filepath.Join(content, filepath.FromSlash(a.ref)))
		if err != nil {
			return "", fmt.Errorf("%s: 参考图: %w", a.rel, err)
		}
		ref = "sha256:" + h
	}
	return hashOf("image", a.kind, a.art.Prompt, ref), nil
}

// ── gen.lock ──────────────────────────────────────────

// locks：gen.lock 所在目录 → 该目录内的相对路径 → 输入 hash。
type locks map[string]map[string]string

const lockName = "gen.lock"

func loadLocks(content string, assets []*asset) (locks, error) {
	l := locks{}
	for _, a := range assets {
		if _, ok := l[a.lock]; ok {
			continue
		}
		m := map[string]string{}
		l[a.lock] = m
		f, err := os.Open(filepath.Join(content, filepath.FromSlash(a.lock), lockName))
		if errors.Is(err, os.ErrNotExist) {
			continue
		} else if err != nil {
			return nil, err
		}
		sc := bufio.NewScanner(f)
		for n := 1; sc.Scan(); n++ {
			h, p, ok := strings.Cut(sc.Text(), "  ")
			if !ok {
				f.Close()
				return nil, fmt.Errorf("%s/%s:%d: 格式错误", a.lock, lockName, n)
			}
			m[p] = h
		}
		f.Close()
		if err := sc.Err(); err != nil {
			return nil, err
		}
	}
	return l, nil
}

func (l locks) save(content, dir string) error {
	p := filepath.Join(content, filepath.FromSlash(dir), lockName)
	m := l[dir]
	if len(m) == 0 {
		if err := os.Remove(p); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
		return nil
	}
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var b strings.Builder
	for _, k := range keys {
		fmt.Fprintf(&b, "%s  %s\n", m[k], k)
	}
	return os.WriteFile(p, []byte(b.String()), 0o644)
}

func lockKey(a *asset) string { return strings.TrimPrefix(a.rel, a.lock+"/") }

type lockEntry struct{ dir, key string }

func (e lockEntry) rel() string { return path.Join(e.dir, e.key) }

// orphans 列出 gen.lock 里有记录、但已不在内容描述里的文件。
func orphans(l locks, assets []*asset) []lockEntry {
	known := map[string]bool{}
	for _, a := range assets {
		known[a.rel] = true
	}
	var out []lockEntry
	for dir, m := range l {
		for k := range m {
			if e := (lockEntry{dir, k}); !known[e.rel()] {
				out = append(out, e)
			}
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].rel() < out[j].rel() })
	return out
}

// stale 返回缺失或过期的文件；图片的同目录参考图过期时，它也跟着过期。
func stale(content string, assets []*asset, l locks) (map[string]string, error) {
	out := map[string]string{} // rel → 原因
	for _, a := range assets {
		if _, err := os.Stat(filepath.Join(content, filepath.FromSlash(a.rel))); err != nil {
			out[a.rel] = "缺失"
			continue
		}
		h, err := inputHash(content, a)
		if err != nil {
			return nil, err
		}
		if l[a.lock][lockKey(a)] != h {
			out[a.rel] = "过期"
		}
	}
	propagateDeps(assets, out, "参考图过期")
	return out, nil
}

// propagateDeps 把同目录参考图在 set 里的图片也加进 set。
func propagateDeps(assets []*asset, set map[string]string, reason string) {
	for changed := true; changed; {
		changed = false
		for _, a := range assets {
			if _, ok := set[a.rel]; ok || a.speech() || !a.refIsDep() {
				continue
			}
			if _, ok := set[a.ref]; ok {
				set[a.rel] = reason
				changed = true
			}
		}
	}
}

// checkAssets 是 chalk build 的前置检查：生成文件齐全、与输入一致，且没有多余的。
func checkAssets(content string, assets []*asset) error {
	l, err := loadLocks(content, assets)
	if err != nil {
		return err
	}
	st, err := stale(content, assets, l)
	if err != nil {
		return err
	}
	var problems []string
	for _, a := range assets {
		if why, ok := st[a.rel]; ok {
			problems = append(problems, why+" "+a.rel)
		}
	}
	for _, e := range orphans(l, assets) {
		problems = append(problems, "多余 "+e.rel())
	}
	if len(problems) == 0 {
		return nil
	}
	const show = 20
	msg := strings.Join(problems[:min(len(problems), show)], "\n  ")
	if len(problems) > show {
		msg += fmt.Sprintf("\n  …共 %d 个", len(problems))
	}
	return fmt.Errorf("生成文件与内容不一致，先运行 chalk gen：\n  %s", msg)
}

// ── chalk gen ─────────────────────────────────────────

func runGen(root string, args []string) error {
	fs := flag.NewFlagSet("gen", flag.ContinueOnError)
	redo := fs.Bool("redo", false, "重新生成指定的文件，即使没有过期")
	lockOnly := fs.Bool("lock", false, "不生成，把现有文件按当前输入记入 gen.lock（接受手工修改）")
	fs.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: chalk gen [--redo | --lock] [路径…]   路径相对 content/，可以是目录或文件")
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *redo && *lockOnly {
		return errors.New("--redo 和 --lock 不能同时用")
	}
	var filters []string
	for _, p := range fs.Args() {
		p = strings.TrimPrefix(path.Clean(filepath.ToSlash(p)), "content/")
		filters = append(filters, p)
	}
	if *redo && len(filters) == 0 {
		return errors.New("--redo 需要指定路径")
	}
	match := func(rel string) bool {
		if len(filters) == 0 {
			return true
		}
		for _, f := range filters {
			if rel == f || strings.HasPrefix(rel, f+"/") {
				return true
			}
		}
		return false
	}

	content := filepath.Join(root, "content")
	var lib librarySrc
	if err := readYAML(filepath.Join(content, "library.yaml"), &lib); err != nil {
		return err
	}
	assets, err := collectAssets(content, &lib)
	if err != nil {
		return err
	}
	l, err := loadLocks(content, assets)
	if err != nil {
		return err
	}
	var mu sync.Mutex // 保护 l 和 gen.lock 文件
	record := func(a *asset) error {
		h, err := inputHash(content, a)
		if err != nil {
			return err
		}
		mu.Lock()
		defer mu.Unlock()
		l[a.lock][lockKey(a)] = h
		return l.save(content, a.lock)
	}

	for _, e := range orphans(l, assets) {
		if !match(e.rel()) {
			continue
		}
		if err := os.Remove(filepath.Join(content, filepath.FromSlash(e.rel()))); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
		delete(l[e.dir], e.key)
		if err := l.save(content, e.dir); err != nil {
			return err
		}
		fmt.Println("removed", e.rel())
	}

	if *lockOnly {
		n := 0
		for _, a := range assets {
			if !match(a.rel) {
				continue
			}
			if _, err := os.Stat(filepath.Join(content, filepath.FromSlash(a.rel))); err != nil {
				fmt.Println("missing", a.rel)
				continue
			}
			if err := record(a); err != nil {
				return err
			}
			n++
		}
		fmt.Printf("locked %d files\n", n)
		return nil
	}

	st, err := stale(content, assets, l)
	if err != nil {
		return err
	}
	work := map[string]string{}
	for _, a := range assets {
		if !match(a.rel) {
			continue
		}
		if *redo {
			work[a.rel] = "redo"
		} else if why, ok := st[a.rel]; ok {
			work[a.rel] = why
		}
	}
	propagateDeps(assets, work, "参考图重画")
	if len(work) == 0 {
		fmt.Println("nothing to generate")
		return nil
	}

	t := newTTS()
	logs := filepath.Join(root, "build", "gen-logs")
	needSpeech := false
	for _, a := range assets {
		if _, ok := work[a.rel]; ok && a.speech() {
			needSpeech = true
		}
	}
	if needSpeech && t.apiKey == "" {
		return errors.New("要合成朗读，需要设置 TTS_API_KEY 或 CHALK_TTS_API_KEY")
	}
	if err := os.MkdirAll(logs, 0o755); err != nil {
		return err
	}

	jobs := 6
	if v, err := strconv.Atoi(os.Getenv("JOBS")); err == nil && v > 0 {
		jobs = v
	}
	sem := make(chan struct{}, jobs)
	type result struct {
		ch chan struct{} // 完成时关闭
		ok bool          // 关闭前写入
	}
	done := map[string]*result{}
	for rel := range work {
		done[rel] = &result{ch: make(chan struct{})}
	}
	var wg sync.WaitGroup
	var failed []string
	for _, a := range assets {
		if _, ok := work[a.rel]; !ok {
			continue
		}
		wg.Add(1)
		go func(a *asset) {
			defer wg.Done()
			r := done[a.rel]
			defer close(r.ch)
			if dep, waits := done[a.ref]; waits && !a.speech() {
				if <-dep.ch; !dep.ok {
					mu.Lock()
					failed = append(failed, a.rel+"（参考图生成失败）")
					mu.Unlock()
					return
				}
			}
			sem <- struct{}{}
			defer func() { <-sem }()
			out := filepath.Join(content, filepath.FromSlash(a.rel))
			var err error
			if a.speech() {
				err = t.synth(a.text, out)
			} else {
				err = drawArt(content, logs, a)
			}
			if err == nil {
				err = record(a)
			}
			if err != nil {
				mu.Lock()
				failed = append(failed, fmt.Sprintf("%s: %v", a.rel, err))
				mu.Unlock()
				fmt.Printf("FAIL %s\n", a.rel)
				return
			}
			r.ok = true
			fmt.Printf("ok %s（%s）\n", a.rel, work[a.rel])
		}(a)
	}
	wg.Wait()
	if len(failed) > 0 {
		sort.Strings(failed)
		return fmt.Errorf("%d 个失败：\n  %s", len(failed), strings.Join(failed, "\n  "))
	}
	return nil
}

// ── 插图 ─────────────────────────────────────────────

const artStyle = "纯黑粗线稿、白底，不要灰色、阴影、渐变、填色或文字"

func artPrompt(a *asset) string {
	p := strings.TrimSpace(a.art.Prompt)
	switch a.kind {
	case "figure":
		return "用图像生成工具画一张儿童简笔画，横版 4:3，尺寸 1448×1086。画风必须与附件 ref.png 完全一致：" + artStyle +
			"，圆润简洁可爱，线条粗细与参考图相同。画面里只有" + p + "，侧面全身，居中占满画面的大部分，不画地面、背景或其他东西。"
	case "frame":
		return "用图像生成工具编辑附件 ref.png（儿童简笔画，" + artStyle + "）。生成一张动画关键帧：" + p +
			"。新画的部分用与原图相同粗细的线条。除了上述改动部分，其余所有线条的位置、粗细、画布尺寸和构图必须与原图完全一致，尺寸与 ref.png 相同。"
	default:
		return "用图像生成工具画一张儿童绘本插图，横版 4:3，尺寸 1448×1086。画风必须与附件 ref.png 完全一致：" + artStyle +
			"，圆润简洁，线条粗细与参考图相同，留白多。画面里出现的角色和车辆与参考图里的长相保持一致。画面内容：" + p + "。"
	}
}

// drawArt 用 Codex 的图像生成画一张图。Codex 在临时目录里运行，只能看到参考图 ref.png，结果保存为 out.png。
func drawArt(content, logs string, a *asset) error {
	tmp, err := os.MkdirTemp("", "chalk-art-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	ref, err := os.ReadFile(filepath.Join(content, filepath.FromSlash(a.ref)))
	if err != nil {
		return fmt.Errorf("参考图: %w", err)
	}
	if err := os.WriteFile(filepath.Join(tmp, "ref.png"), ref, 0o644); err != nil {
		return err
	}
	logPath := filepath.Join(logs, strings.ReplaceAll(a.rel, "/", "-")+".log")
	log, err := os.Create(logPath)
	if err != nil {
		return err
	}
	defer log.Close()
	cmd := exec.Command("codex", "exec", "--skip-git-repo-check", "-s", "workspace-write", "-i", "ref.png")
	cmd.Dir = tmp
	cmd.Stdin = strings.NewReader(artPrompt(a) + "把结果保存为当前目录下的 out.png。")
	cmd.Stdout, cmd.Stderr = log, log
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("codex: %w（日志 %s）", err, logPath)
	}
	if _, err := loadGray(filepath.Join(tmp, "out.png")); err != nil {
		return fmt.Errorf("没有得到可用的 out.png: %w（日志 %s）", err, logPath)
	}
	data, err := os.ReadFile(filepath.Join(tmp, "out.png"))
	if err != nil {
		return err
	}
	dst := filepath.Join(content, filepath.FromSlash(a.rel))
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	return os.WriteFile(dst, data, 0o644)
}
