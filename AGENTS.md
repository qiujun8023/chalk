# Agent Guide

给在本仓库工作的 AI 代理。项目说明见 [README.md](README.md)。

- 改了 `content/` 或 `tools/` 后运行 `cd tools && go vet . && go build -o chalk . && ./chalk build`，并查看 `build/preview/` 里受影响的帧。
- 改了固件后在 `firmware/` 下 `idf.py build`；设备连着时用 `tools/devkeys.py` 实机验证。
- `firmware/main/app/library.cc` 解析的字段与 `tools/build.go` 输出的 `library.json` 必须一致，两边一起改。
- `tools/build.go` 的 `storageSize` 与 `firmware/partitions.csv` 的 storage 分区大小一致。
- 内容画面（故事页、车辆）不加文字（车身上本来就有的字除外），内容靠朗读；组列表和设置页用文字列表。文案以 slate 的工程车文案为准。
- TTS key 在线上 slate 的部署配置里，只通过环境变量传给 `chalk build`，不写进仓库或文件。
- 提交信息不加 AI 署名或 Co-Authored-By。
