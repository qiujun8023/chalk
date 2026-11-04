package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// 设备端只解码 Ogg Opus：24 kHz 单声道 32 kbps，约 4 KB/s。
func encodeOpus(args []string, out string) error {
	full := append([]string{"-hide_banner", "-loglevel", "error", "-y"}, args...)
	full = append(full, "-ac", "1", "-ar", "24000", "-c:a", "libopus", "-b:a", "32k", "-f", "ogg", out)
	cmd := exec.Command("ffmpeg", full...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("ffmpeg: %w: %s", err, strings.TrimSpace(stderr.String()))
	}
	return nil
}

type tts struct {
	cacheDir string
	apiKey   string
	baseURL  string
	model    string
	voice    string
}

// 用 OpenAI 兼容的流式 chat/completions 接口合成（与 slate 相同，默认小米 MiMo、音色冰糖）。
// 缓存命中时不需要 TTS_API_KEY。
func newTTS(cacheDir string) *tts {
	return &tts{
		cacheDir: cacheDir,
		apiKey:   os.Getenv("TTS_API_KEY"),
		baseURL:  strings.TrimRight(envOr("TTS_BASE_URL", "https://api.xiaomimimo.com/v1"), "/"),
		model:    envOr("TTS_MODEL", "mimo-v2.5-tts"),
		voice:    envOr("TTS_VOICE", "冰糖"),
	}
}

func (t *tts) provider() string { return t.model + ":" + t.voice + ":ogg24k32" }

// speak 返回缓存里的 ogg 路径，相同文本和音色只合成一次。
func (t *tts) speak(text string) (string, error) {
	text = strings.TrimSpace(text)
	if text == "" {
		return "", errors.New("朗读文本为空")
	}
	out := filepath.Join(t.cacheDir, hashOf(t.provider(), text)+".ogg")
	if _, err := os.Stat(out); err == nil {
		return out, nil
	}
	if err := os.MkdirAll(t.cacheDir, 0o755); err != nil {
		return "", err
	}
	if t.apiKey == "" {
		return "", errors.New("缓存未命中，需要设置 TTS_API_KEY")
	}
	tmp := out + ".tmp.ogg"
	if err := t.speakRemote(text, tmp); err != nil {
		os.Remove(tmp)
		return "", fmt.Errorf("TTS %q: %w", text, err)
	}
	return out, os.Rename(tmp, out)
}

func (t *tts) speakRemote(text, out string) error {
	body, _ := json.Marshal(map[string]any{
		"model":    t.model,
		"messages": []map[string]string{{"role": "user", "content": ""}, {"role": "assistant", "content": text}},
		"audio":    map[string]string{"format": "pcm16", "voice": t.voice},
		"stream":   true,
	})
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, t.baseURL+"/chat/completions", bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+t.apiKey)
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode/100 != 2 {
		return fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	var pcm []byte
	finish := ""
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 64<<10), 8<<20)
	for sc.Scan() {
		payload, ok := strings.CutPrefix(strings.TrimSpace(sc.Text()), "data:")
		if payload = strings.TrimSpace(payload); !ok || payload == "" || payload == "[DONE]" {
			continue
		}
		var chunk struct {
			Choices []struct {
				Delta struct {
					Audio struct {
						Data string `json:"data"`
					} `json:"audio"`
				} `json:"delta"`
				FinishReason *string `json:"finish_reason"`
			} `json:"choices"`
		}
		if err := json.Unmarshal([]byte(payload), &chunk); err != nil {
			return err
		}
		if len(chunk.Choices) == 0 {
			continue
		}
		if fr := chunk.Choices[0].FinishReason; fr != nil {
			finish = *fr
		}
		if d := chunk.Choices[0].Delta.Audio.Data; d != "" {
			b, err := base64.StdEncoding.DecodeString(d)
			if err != nil {
				return err
			}
			pcm = append(pcm, b...)
		}
	}
	if err := sc.Err(); err != nil {
		return err
	}
	if finish != "stop" || len(pcm) == 0 {
		return fmt.Errorf("音频流未正常结束 finish=%q bytes=%d", finish, len(pcm))
	}
	raw := out + ".pcm"
	defer os.Remove(raw)
	if err := os.WriteFile(raw, pcm, 0o644); err != nil {
		return err
	}
	return encodeOpus([]string{"-f", "s16le", "-ar", "24000", "-ac", "1", "-i", raw}, out)
}

// 调音量时的提示音：短促的“叮”。
func volumeBeep(out string) error {
	return encodeOpus([]string{"-f", "lavfi", "-i", "sine=frequency=1046:duration=0.18", "-af", "volume=0.6,afade=t=out:st=0.06:d=0.12"}, out)
}

func envOr(k, def string) string { return orDefault(os.Getenv(k), def) }

func orDefault(v, def string) string {
	if v == "" {
		return def
	}
	return v
}
