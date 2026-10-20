#!/bin/sh
set -eu
: "${SBE_JAR:?Set SBE_JAR to sbe-all-1.35.0.jar}"
JAVA_CMD=${JAVA_CMD:-java}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
EXPECTED=d11f4bb7c71322e3538613b2da18b11625be400465b29a09db80bc9785cea3ab
ACTUAL=$(shasum -a 256 "$SBE_JAR" | cut -d ' ' -f 1)
[ "$ACTUAL" = "$EXPECTED" ] || { echo "SBE jar checksum mismatch" >&2; exit 1; }
"$JAVA_CMD" --add-opens java.base/jdk.internal.misc=ALL-UNNAMED \
  -Dsbe.target.language=Cpp -Dsbe.target.namespace=fq_sbe \
  -Dsbe.decode.unknown.enum.values=true -Dsbe.output.dir="$ROOT/generated/cme" \
  -jar "$SBE_JAR" "$ROOT/schemas/cme/templates_v13.xml"
# 仅修正生成器输出的 C++11 语法兼容性；官方 schema 与字段偏移完全不变。
python3 "$ROOT/tools/sbe_gcc48_compat.py" --apply
python3 "$ROOT/tools/generate_layout.py"
