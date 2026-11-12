# Agent Guide

给在本仓库工作的 AI 代理。项目说明见 [README.md](README.md)。

- 改了 `content/` 或 `tools/` 后运行 `cd tools && go vet . && go build -o chalk . && ./chalk build`，并查看 `build/preview/` 里受影响的帧。
- 改了固件后在 `firmware/` 下 `idf.py build`；设备连着时用 `tools/devkeys.py` 实机验证。
- `firmware/main/app/library.cc` 解析的字段与 `tools/build.go` 输出的 `library.json` 必须一致，两边一起改；固件新增写死的中文时同步 `build.go` 的 `firmwareLabels`。
- 内容画面（故事和古诗的页、车辆和动物形象）不加文字（车身上本来就有的字除外），内容靠朗读；列表和设置页用文字列表。
- TTS key 是 `~/.config/secrets/env` 的 `CHALK_TTS_API_KEY`，加载该文件后运行 `chalk build`；不写进仓库。
- `content/tts/` 入库，CI 不配 TTS key；改了朗读文案后把 `content/tts/` 的新增和删除一起提交。
- 提交信息不加 AI 署名或 Co-Authored-By。
