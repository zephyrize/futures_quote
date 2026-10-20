#!/usr/bin/env bash
# 与压缩包放在同一目录执行。先完整解到临时目录，再替换 futures_quote；旧目录保留备份。
# config/local.yaml 和 local-*.yaml 是服务器现场配置，升级时保留，避免覆盖网卡与源 IP。
set -euo pipefail
base=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
archive=${1:-"$base/futures_quote-offline.tar.gz"}
[[ -f $archive ]] || { echo "压缩包不存在: $archive" >&2; exit 1; }
if [[ -f $archive.sha256 ]]; then
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$(dirname "$archive")" && sha256sum -c "${archive##*/}.sha256")
    elif command -v shasum >/dev/null 2>&1; then
        (cd "$(dirname "$archive")" && shasum -a 256 -c "${archive##*/}.sha256")
    else
        echo '找不到 SHA256 校验工具，拒绝部署。' >&2
        exit 1
    fi
else
    echo '未找到 .sha256 校验文件，将只校验 tar 完整性。' >&2
fi
# 旧版 macOS 包可能夹带 ._ AppleDouble 元数据；仅忽略这些项，普通文件仍必须
# 位于 futures_quote/。先检查路径再忽略，防止伪装的元数据条目绕过目录约束。
while IFS= read -r entry; do
    case "/$entry/" in
        */../*|*/./*) echo "压缩包包含不安全路径: $entry" >&2; exit 1 ;;
    esac
    if [[ ${entry##*/} == ._* ]]; then
        case "$entry" in
            ._futures_quote|futures_quote/*) continue ;;
        esac
    fi
    case "$entry" in
        futures_quote|futures_quote/*) ;;
        *) echo "压缩包包含意外路径: $entry" >&2; exit 1 ;;
    esac
done < <(tar -tzf "$archive")
target="$base/futures_quote"
[[ ! -L $target ]] || { echo "拒绝覆盖符号链接: $target" >&2; exit 1; }
stage=$(mktemp -d "$base/.fq-unpack.XXXXXX")
trap 'rm -rf "$stage"' EXIT
tar --exclude='._*' --exclude='*/._*' -xzf "$archive" -C "$stage"
[[ -f $stage/futures_quote/src/client.cpp && -f $stage/futures_quote/apps/fq_dump.cpp &&
   -x $stage/futures_quote/scripts/build.sh && -x $stage/futures_quote/scripts/run.sh ]] || {
    echo '压缩包缺少必要项目文件。' >&2; exit 1;
}
for config_file in "$target/config/local.yaml" "$target"/config/local-*.yaml; do
    # 现场 YAML 只复制普通文件；不跟随符号链接读取工作目录外的内容。
    if [[ -f $config_file && ! -L $config_file ]]; then
        cp -p "$config_file" "$stage/futures_quote/config/${config_file##*/}"
    fi
done
backup=''
if [[ -e $target ]]; then
    backup="$base/futures_quote.backup-$(date +%Y%m%d-%H%M%S)-$$"
    mv "$target" "$backup"
fi
if ! mv "$stage/futures_quote" "$target"; then
    [[ -z $backup ]] || mv "$backup" "$target"
    echo '替换失败，已尝试恢复旧目录。' >&2
    exit 1
fi
echo "解压完成：$target"
[[ -z $backup ]] || echo "旧目录备份：$backup"
echo '下一步：cd futures_quote && ./scripts/build.sh'
