#!/usr/bin/env bash
# 服务器运行入口。先做本地配置检查，再按指定时长收包；不执行任何联网下载操作。
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
config="$root/config/local.yaml"
duration=60
rate=100
check_only=0
trace=0
usage() {
    echo "用法: $0 [--config YAML] [--check] [--duration 秒] [--rate 每秒最多打印事件数] [--trace]" >&2
    echo '默认读取 config/local.yaml，运行 60 秒，每秒最多打印 100 条普通事件；--rate 0 不采样。' >&2
    echo '--trace 输出建组信息及前 20 个 UDP 收包结果，便于区分内核未交付和配置路由不匹配。' >&2
}
while (($#)); do
    case "$1" in
        --config) (($# >= 2)) || { usage; exit 2; }; config=$2; shift 2 ;;
        --duration) (($# >= 2)) || { usage; exit 2; }; duration=$2; shift 2 ;;
        --rate) (($# >= 2)) || { usage; exit 2; }; rate=$2; shift 2 ;;
        --check) check_only=1; shift ;;
        --trace) trace=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done
[[ $duration =~ ^[0-9]+$ && $rate =~ ^[0-9]+$ ]] || { usage; exit 2; }
[[ -f $config ]] || { echo "配置文件不存在: $config。请先复制并填写 config/local.yaml。" >&2; exit 1; }
binary="$root/build-server/bin/fq_dump"
[[ -x $binary ]] || { echo '请先执行 ./scripts/build.sh' >&2; exit 1; }
"$binary" --config "$config" --check-config
if ((check_only)); then
    exit 0
fi
echo "开始接收：配置=$config，持续=${duration}s，普通事件打印上限=${rate}/s" >&2
if ((trace)); then
    export FQ_TRACE_TRANSPORT=1
fi
"$binary" --config "$config" --duration "$duration" --rate "$rate"
