#!/usr/bin/env bash
# CentOS 7 离线构建入口。Linux 直接用本机 g++/ar，不调用 yum、curl、git 或 CMake。
# 对象文件统一写入 build-server；动态库和静态库使用同一批 C++11 PIC 对象。
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $(uname -s) != Linux ]]; then
    echo 'build.sh 的离线 g++ 构建面向 Linux；本机 macOS 请使用 CMake 构建。' >&2
    exit 1
fi
cxx=${CXX:-g++}
ar_bin=${AR:-ar}
command -v "$cxx" >/dev/null || { echo "缺少 C++ 编译器: $cxx" >&2; exit 1; }
command -v "$ar_bin" >/dev/null || { echo "缺少归档工具: $ar_bin" >&2; exit 1; }
build="$root/build-server"
mkdir -p "$build/obj" "$build/lib" "$build/bin"
common=(-std=c++11 -O2 -pthread -fPIC -fvisibility=hidden -fvisibility-inlines-hidden
        -Wall -Wextra -Wpedantic -DYAML_CPP_STATIC_DEFINE -DYAML=fq_private_yaml
        -I"$root/include" -I"$root/src" -I"$root/generated/cme"
        -I"$root/third_party/yaml-cpp-0.8.0/include"
        -I"$root/third_party/yaml-cpp-0.8.0/src")
objects=()
for source in client config registry cme lme transport pcap; do
    obj="$build/obj/$source.o"
    echo "[CXX] src/$source.cpp"
    "$cxx" "${common[@]}" -DFQ_BUILD -D_GNU_SOURCE -c "$root/src/$source.cpp" -o "$obj"
    objects+=("$obj")
done
for src in "$root"/third_party/yaml-cpp-0.8.0/src/*.cpp; do
    name=${src##*/}
    obj="$build/obj/yaml-${name%.cpp}.o"
    echo "[CXX] yaml-cpp/$name"
    "$cxx" "${common[@]}" -c "$src" -o "$obj"
    objects+=("$obj")
done
rm -f "$build/lib/libfutures_quote.a" "$build/lib/libfutures_quote.so" "$build/lib/libfutures_quote.so.1"
"$ar_bin" rcs "$build/lib/libfutures_quote.a" "${objects[@]}"
"$cxx" -shared -pthread -Wl,-soname,libfutures_quote.so.1 \
    -Wl,--version-script="$root/cmake/exports.map" \
    -o "$build/lib/libfutures_quote.so.1" "${objects[@]}"
ln -s libfutures_quote.so.1 "$build/lib/libfutures_quote.so"
"$cxx" -std=c++11 -O2 -pthread -I"$root/include" "$root/apps/fq_dump.cpp" \
    "$build/lib/libfutures_quote.a" -o "$build/bin/fq_dump"
"$cxx" -std=c++11 -O2 -pthread -I"$root/include" -I"$root/tests" \
    "$root/apps/fq_bench.cpp" "$build/lib/libfutures_quote.a" -o "$build/bin/fq_bench"
"$cxx" -std=c++11 -O2 -pthread -I"$root/include" -I"$root/src" \
    "-DFQ_SOURCE_DIR=\"$root\"" "$root/tests/test_main.cpp" \
    "$build/lib/libfutures_quote.a" -o "$build/bin/fq_tests"
echo '[TEST] 协议与恢复单元测试（仅本地样本，不访问网络）'
"$build/bin/fq_tests"
echo '[TEST] 配置解析与可执行文件'
"$build/bin/fq_dump" --config "$root/config/replay.yaml" --check-config
echo "构建完成：$build/bin/fq_dump，$build/lib/libfutures_quote.{a,so.1}"
