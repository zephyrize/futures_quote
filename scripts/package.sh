#!/usr/bin/env bash
# 在本地生成服务器离线源码包。只复制 build.sh 所需源码、许可、测试及运行模板；
# 开发文档、协议原文、生成器、第三方自测及本地构建产物不进入测试机。
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# 离线包直接带已生成头文件，打包前阻止旧版 GCC 无法编译的生成写法回流。
python3 "$root/tools/sbe_gcc48_compat.py" --check
out=${1:-"$root/output"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
stage=$(mktemp -d "$out/.fq-stage.XXXXXX")
trap 'rm -rf "$stage"' EXIT
package="$stage/futures_quote"
mkdir -p "$package"/{apps,cmake,config,scripts,tests,third_party/yaml-cpp-0.8.0}
cp -R "$root/src" "$root/include" "$root/generated" "$package/"
cp -R "$root/third_party/yaml-cpp-0.8.0/include" \
      "$root/third_party/yaml-cpp-0.8.0/src" "$package/third_party/yaml-cpp-0.8.0/"
cp "$root/third_party/yaml-cpp-0.8.0/LICENSE" "$package/third_party/yaml-cpp-0.8.0/"
cp "$root/third_party/SBE-LICENSE" "$package/third_party/"
cp "$root/cmake/exports.map" "$package/cmake/"
cp "$root/apps/fq_dump.cpp" "$root/apps/fq_bench.cpp" "$package/apps/"
cp "$root/tests/test_main.cpp" "$root/tests/wire_fixture.hpp" "$package/tests/"
cp -p "$root/scripts/build.sh" "$root/scripts/run.sh" "$package/scripts/"
for file in replay.yaml benchmark.yaml cme-382-384-test.yaml lme-307-807-test.yaml lme-112-612-614-test.yaml; do
    cp "$root/config/$file" "$package/config/$file"
done
archive="$out/futures_quote-offline.tar.gz"
# macOS 的 bsdtar 默认把 xattr 写成 PAX 扩展头和 ._ AppleDouble 文件；
# GNU tar 会把后者当成真实路径。明确关闭两者，使离线包在 CentOS 7 可直接解开。
COPYFILE_DISABLE=1 tar --no-xattrs -czf "$archive.tmp" -C "$stage" futures_quote
mv -f "$archive.tmp" "$archive"
cp "$root/scripts/unpack.sh" "$out/unpack.sh"
chmod +x "$out/unpack.sh"
if command -v sha256sum >/dev/null 2>&1; then
    (cd "$out" && sha256sum "${archive##*/}") > "$archive.sha256"
else
    (cd "$out" && shasum -a 256 "${archive##*/}") > "$archive.sha256"
fi
echo "打包完成：$archive"
echo "同时上传：$out/unpack.sh 和 $archive.sha256"
