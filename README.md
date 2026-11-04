# Chalk

Chalk（粉笔）是跑在 ZecTrix Note4（ESP32-S3 + 4.2" 400×300 黑白墨水屏）上的儿童绘本和工程车动画。

内容在 Mac 上编译成 LittleFS 镜像，和固件一起烧进设备。设备不联网、没有后端，只负责显示和播放。

## 玩法

内容按组组织，平时显示某一组的当前页或当前车。内容画面里不放文字，内容靠朗读；组列表和设置页是文字列表。

- **工程车**：12 辆车，翻到时念一句，同时播一遍关键帧动画；念完、动画播完就停在这一辆。
- **绘本**：每页一张插图配一段旁白，念完停 1.5 秒翻到下一页，播到最后一页停下。

有哪些组、顺序如何，见 `content/library.yaml`。

## 按键

| 操作 | 行为 |
| --- | --- |
| 上 / 下 短按 | 上一组 / 下一组：念组名，从第一页开始播，到头循环 |
| 确认 短按 | 组内下一个（页或车），到头循环 |
| 上 + 下 同按 | 进入 / 退出组列表 |
| 三键同按 | 进入 / 退出设置 |

- 长按和短按效果相同。上下同时按住时再按下确认就是三键；没有按确认，上下都松开时进入组列表。
- 动画播放中按键会立即停下动画，执行这次按键。
- 刷屏期间连按的上下键累加步数（确认键同理），一次跳到目标、只刷一屏；其他按键只执行最后一个。
- 闲置 3 分钟且没插 USB 时深睡，画面保留。确认键或下键唤醒，唤醒的那次按键直接执行；上键不能唤醒。
- 深睡前保存每组的位置、音量和屏幕上的整帧，唤醒后直接局刷。

### 组列表

第一行「返回」，之后每组一行，进入时光标停在当前组。上 / 下移动光标并念出组名，确认进入那一组（选「返回」则回到原来的内容接着播）。一屏 9 行，放不下时按页显示。

### 设置

| 行 | 确认 |
| --- | --- |
| 返回 | 回到原来的内容接着播 |
| 音量 | 进入编辑：上 / 下调 1–10 档，每调一档用新音量响一声，再按确认结束 |
| 刷新屏幕 | 全刷一次清残影 |
| 立即休眠 | 回到内容画面后深睡 |
| 重置进度 | 第一次按变成「再按确认重置」，再按一次各组回到第一页、音量恢复默认；移开光标即取消 |

底部只读显示电量（百分比、电压、充电状态）、存储用量、内容和固件的构建时间。进入时光标停在「音量」。

组列表和设置页 30 秒无操作自动回到内容画面。

## 状态栏

顶部 24px，布局与字体取自 slate（Zfull 16px 标题、12px 数字），底部一条分隔线。

| 位置 | 内容 |
| --- | --- |
| 左 | 组内进度，如 `3/12` |
| 中 | 车名或绘本名；组列表显示 `目录`，设置页显示 `设置` |
| 右 | 电量百分比 + 三格电池；充电时百分比换成闪电，充满显示 `100%` |

电量每分钟读一次，按 10% 取整；插拔 USB、充满时立即更新。状态栏有变化才局刷。

## 刷新

| 类型 | 实测耗时 | 用途 |
| --- | --- | --- |
| 局刷 | 约 0.8 s | 默认：翻页、换车、动画关键帧 |
| 全刷 | 约 1.2 s | 开机首帧；连续局刷 8 次后的下一次切换；设置页「刷新屏幕」 |

声音先于刷屏开始，刷新的等待被朗读盖住。

## 目录

| 目录 | 内容 |
| --- | --- |
| `firmware/` | ESP-IDF 5.5 固件 |
| `tools/` | Go 内容编译器 `chalk`、字库源（`fonts/`，取自 slate）、Codex 生图脚本（`art/`）、串口调试脚本 |
| `content/` | 源内容：线稿、关键帧、文案 |
| `build/` | 编译产物（不入库）：`fs/`、`storage.bin`、`preview/`、生图日志 `art-logs/` |

## 构建与烧录

需要 Go、ffmpeg（带 libopus）、Python 3.11–3.14（首次构建会在 `build/pyenv` 装 littlefs-python）、ESP-IDF 5.5。

```bash
cd tools && go build -o chalk . && TTS_API_KEY=... ./chalk build
cd ../firmware && idf.py -p /dev/cu.usbmodem2101 flash
```

- `chalk build` 生成 `build/storage.bin`，`idf.py flash` 会把它一起烧到 storage 分区；只改固件时用 `idf.py app-flash`。
- 重新烧内容镜像会清掉设备上保存的位置和音量。
- `build/preview/` 里有每一帧的 PNG 和每段动画的 GIF，不用烧录就能检查效果。

### TTS

朗读用 OpenAI 兼容的流式 `chat/completions` 接口合成，与 slate 相同。

| 变量 | 默认 |
| --- | --- |
| `TTS_API_KEY` | 必填；缓存都命中时可不填 |
| `TTS_BASE_URL` | `https://api.xiaomimimo.com/v1` |
| `TTS_MODEL` | `mimo-v2.5-tts` |
| `TTS_VOICE` | `冰糖` |

合成结果按文本缓存在 `content/.cache/tts/`。音频统一转成 24 kHz 单声道 32 kbps 的 Ogg Opus。

## 内容格式

`content/library.yaml` 决定组的顺序，每项对应 `content/<名称>/item.yaml`。

```yaml
type: story            # story | vehicles
title: 工地的一天        # 状态栏标题，切到这一组时朗读
pages:
  - image: p01.png
    say: 清晨，太阳出来了，工地上静悄悄的。
```

```yaml
type: vehicles
title: 工程车
vehicles:
  - id: excavator      # content/vehicles/excavator/，含 base.png 和关键帧
    name: 挖掘机
    intro: 挖掘机，挖呀挖。轰隆隆！   # 翻到时朗读，同时播放 actions
    actions:
      - frames:
          - {image: k1.png, region: [0, 40, 690, 1046], ms: 400}
          - {image: base.png}
```

- 图片是白底纯黑线稿，任意尺寸，裁白边后等比缩放进状态栏下方区域，按 128 二值化成 1bpp。
- `region` 是源图像素 `[x, y, w, h]`：关键帧只取这个区域，其余用 `base.png`。AI 改图时不动的部分会有细微偏移，靠它保证逐像素一致，局刷时车身不闪。同一辆车的所有帧共用一个缩放位置。
- `ms` 是这一帧刷完后的停留时间。

### 生成插图和关键帧

用 Codex 的图像生成，基于已有线稿改图或参考画风，保持风格一致：

```bash
tools/art/gen.sh vehicles   # 按 tools/art/vehicles.tsv 生成车辆关键帧
tools/art/gen.sh stories    # 按 tools/art/stories.tsv 生成绘本插图：同一本书按页顺序画，后面的页参考第 1 页保持角色一致
tools/chalk region base.png k1.png out.png   # 建议关键帧的 region，并输出标注图
```

两个 tsv 的列格式见 `gen.sh` 开头的注释。已存在的文件会跳过；想重画某一张，删掉后再运行。

新增一本绘本：在 `stories.tsv` 里按页写画面描述，生成插图后新建 `content/<名称>/item.yaml` 写旁白，加进 `library.yaml`。页数不限。

## 调试

固件的日志和调试输入都走 USB Serial/JTAG。`tools/devkeys.py` 发送模拟按键并打印日志，注意打开串口会复位芯片：

```bash
cd firmware && python ../tools/devkeys.py 'e 3 e 5 d 3 i'   # 字母是按键，数字是等待秒数
```

| 键 | 含义 |
| --- | --- |
| `u` `d` `e` | 上、下、确认短按 |
| `l` | 上 + 下（组列表） |
| `m` | 三键（设置） |
| `s` | 立即深睡 |
| `i` | 打印状态 |
| `p` | 导出屏幕当前帧；加 `--dump <前缀>` 保存为 PNG |

## 硬件

引脚、电源时序、EPD 和 ES8311 的细节与 slate 相同，见 slate 的 `firmware/README.md`。分区：

| 分区 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | 24 KB | 未使用 |
| `factory` | `0x10000` | 2 MB | 固件 |
| `storage` | `0x210000` | 约 14 MB | LittleFS：内容、`state.json`、`screen.bin` |
