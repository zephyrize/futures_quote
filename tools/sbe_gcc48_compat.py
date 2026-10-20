#!/usr/bin/env python3
"""修正 SBE 生成器的 GCC 4.8 自动类型推导写法，不改变协议字段或布局。"""

import argparse
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "generated/cme/fq_sbe"
# SBE 1.35.0 生成的 auto &group { groupAccessor() }; 在 GCC 4.8 上被误推导成
# initializer_list，随后的 hasNext()/next() 编译失败。等号初始化保留引用语义。
INCOMPATIBLE = re.compile(
    r"(?m)^([ \t]*)auto &([A-Za-z_]\w*) \{ ([A-Za-z_]\w*\(\)) \};([ \t]*)$"
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--apply", action="store_true", help="修正生成的头文件")
    mode.add_argument("--check", action="store_true", help="检查是否仍含 GCC 4.8 不兼容写法")
    args = parser.parse_args()
    files = sorted(GENERATED.glob("*.h"))
    if not files:
        parser.error("未找到 SBE 生成头文件")
    total = 0
    for path in files:
        original = path.read_text(encoding="utf-8")
        changed, count = INCOMPATIBLE.subn(r"\1auto &\2 = \3;\4", original)
        total += count
        if args.apply and count:
            path.write_text(changed, encoding="utf-8")
    if args.check and total:
        parser.exit(1, "发现 {} 处 GCC 4.8 不兼容的 SBE 生成写法，请运行 --apply。\n".format(total))
    print("SBE GCC 4.8 compatibility: {} {}".format(total, "rewritten" if args.apply else "remaining"))


if __name__ == "__main__":
    main()
