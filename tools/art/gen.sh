#!/usr/bin/env bash
# 用 Codex 的图像生成画线稿，已存在的文件跳过；想重画某一张，删掉后再运行。
#
#   tools/art/gen.sh vehicles   按 vehicles.tsv 生成车辆关键帧：<车> <文件名> <改动>，基于该车 base.png 改图
#   tools/art/gen.sh animals    按 animals.tsv 生成动物：<动物> base.png <画面> 画原图（参考车辆线稿的画风），其余行同 vehicles
#   tools/art/gen.sh stories    按 stories.tsv 生成绘本插图：<目录> <文件名> <参考图> <画面>
#   tools/art/gen.sh poems      按 poems.tsv 生成古诗插图，格式同 stories
#
# <目录> 相对 content/，如 stories/rainy-day。参考图：vehicle:<车> 用该车线稿；page:<路径> 用 content 下的某张图；
# first 用本目录的 p01.png。第一列相同的行按顺序画（后面的页参考第 1 页保持角色一致），不同目录并行，并发数 JOBS 默认 6。
# Codex 在临时目录里运行，只能看到参考图；日志在 build/art-logs/。
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
content="$(cd "$here/../../content" && pwd)"
logs="$(cd "$here/../.." && pwd)/build/art-logs"
mode=${1:-}

style="纯黑粗线稿、白底，不要灰色、阴影、渐变、填色或文字"
case $mode in
    vehicles) subject="交通工具" ;;
    animals) subject="动物" ;;
    stories | poems) ;;
    *) echo "usage: $0 vehicles|animals|stories|poems" >&2; exit 2 ;;
esac
list="$here/$mode.tsv"
mkdir -p "$logs"

# draw <参考图> <输出路径> <提示词>：结果保存为 out.png 后移到输出路径。
draw() {
    local ref=$1 dst=$2 prompt=$3 tmp
    tmp=$(mktemp -d)
    cp "$ref" "$tmp/ref.png"
    local log="$logs/$(echo "${dst#"$content"/}" | tr / -).log"
    (cd "$tmp" && echo "$prompt把结果保存为当前目录下的 out.png。" |
        codex exec --skip-git-repo-check -s workspace-write -i ref.png >"$log" 2>&1)
    if [[ -f "$tmp/out.png" ]]; then
        mkdir -p "$(dirname "$dst")" && mv "$tmp/out.png" "$dst" && echo "ok ${dst#"$content"/}"
    else
        echo "FAIL ${dst#"$content"/}（日志 $log）"
    fi
    rm -rf "$tmp"
}

row() {
    local group=$1 out=$2 a=$3 b=${4:-}
    if [[ $mode == animals && $out == base.png ]]; then
        draw "$content/vehicles/bus/base.png" "$content/animals/$group/base.png" \
            "用图像生成工具画一张儿童简笔画，横版 4:3，尺寸 1448×1086。画风必须与附件 ref.png 完全一致：${style}，圆润简洁可爱，线条粗细与参考图相同。画面里只有一只${a}，侧面全身，居中占满画面的大部分，不画地面、背景或其他东西。"
        return
    fi
    if [[ $mode == vehicles || $mode == animals ]]; then
        [[ -f "$(dir_of "$group")/base.png" ]] || { echo "FAIL $group/$out：缺少 base.png"; return; }
        draw "$(dir_of "$group")/base.png" "$(dir_of "$group")/$out" \
            "用图像生成工具编辑附件 ref.png（儿童简笔画风格的${subject}，${style}）。生成一张动画关键帧：${a}。新画的部分用与原图相同粗细的线条。除了上述改动部分，其余所有线条的位置、粗细、画布尺寸和构图必须与原图完全一致，尺寸与 ref.png 相同。"
        return
    fi
    local ref
    case $a in
        vehicle:*) ref="$content/vehicles/${a#vehicle:}/base.png" ;;
        page:*) ref="$content/${a#page:}" ;;
        first) ref="$content/$group/p01.png" ;;
        *) echo "FAIL $group/$out：未知参考图 $a"; return ;;
    esac
    draw "$ref" "$content/$group/$out" \
        "用图像生成工具画一张儿童绘本插图，横版 4:3，尺寸 1448×1086。画风必须与附件 ref.png 完全一致：${style}，圆润简洁，线条粗细与参考图相同，留白多。画面里出现的角色和车辆与参考图里的长相保持一致。画面内容：${b}。"
}

dir_of() {
    case $mode in
        vehicles | animals) echo "$content/$mode/$1" ;;
        *) echo "$content/$1" ;;
    esac
}

run_group() {
    local group=$1
    while IFS=$'\t' read -r g out a b; do
        [[ "$g" != "$group" || -f "$(dir_of "$g")/$out" ]] && continue
        row "$g" "$out" "$a" "$b"
    done <"$list"
}

for group in $(cut -f1 "$list" | awk 'NF && !seen[$0]++'); do
    while (($(jobs -rp | wc -l) >= ${JOBS:-6})); do sleep 2; done
    run_group "$group" &
done
wait
