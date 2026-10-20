#!/usr/bin/env python3
"""只读核对 CME 官方生产 schema；不会自动替换 XML 或生成代码。

默认下载官方 Production XML，也可用 --official-file 比对已下载的文件。
退出码：0=字节完全一致，1=文件存在差异，2=下载、读取或协议身份检查失败。
离线文件的来源由维护者确认；本工具不把文件名当作来源证明。
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import urllib.request
import xml.etree.ElementTree as ET


OFFICIAL_URL = (
    "https://www.cmegroup.com/ftp/SBEFix/Production/Templates/templates_FixBinary.xml"
)
LOCAL_SCHEMA = Path(__file__).resolve().parents[1] / "schemas/cme/templates_v13.xml"


def describe(data):
    """校验协议身份并提取版本和模板清单，防止误用 HTML、iLink 或其他 SBE 产品。"""
    root = ET.fromstring(data)
    if (root.tag.split("}")[-1] != "messageSchema"
            or root.get("package") != "mktdata" or root.get("id") != "1"
            or root.get("byteOrder") != "littleEndian"):
        raise ValueError("expected CME MDP mktdata/schemaId=1/littleEndian XML")
    messages = [m for m in root if m.tag.split("}")[-1] == "message"]
    if not root.get("version") or not messages:
        raise ValueError("schema version or message definitions missing")
    return {
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
        "schema_id": root.get("id"),
        "version": root.get("version"),
        "description": root.get("description"),
        "template_ids": sorted(int(m.attrib["id"]) for m in messages),
    }


def compare(local, candidate):
    """字节一致才能直接返回通过；差异可能只是排版，也可能影响解码，需审阅 XML diff。"""
    return {
        "status": "identical" if local == candidate else "different_review_required",
        "local": describe(local),
        "candidate": describe(candidate),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--local", type=Path, default=LOCAL_SCHEMA)
    parser.add_argument("--official-file", type=Path,
                        help="已从官方取得的候选文件；离线比对不会验证其来源")
    parser.add_argument("--save-candidate", type=Path,
                        help="将下载并通过协议身份检查的候选 XML 保存到指定新文件")
    args = parser.parse_args()
    try:
        local = args.local.read_bytes()
        if args.official_file:
            candidate = args.official_file.read_bytes()
        else:
            # 下载不属于运行时热路径；不关闭 TLS 校验，也不在失败时回退到本地副本。
            with urllib.request.urlopen(OFFICIAL_URL, timeout=20) as response:
                candidate = response.read()
        result = compare(local, candidate)
        result["source"] = str(args.official_file) if args.official_file else OFFICIAL_URL
        result["source_mode"] = "offline_file_unverified_origin" if args.official_file else "official_https"
        if args.save_candidate:
            # 独占创建，避免误覆盖当前 schema 或已有候选文件。
            with args.save_candidate.open("xb") as output:
                output.write(candidate)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result["status"] == "identical" else 1
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        print(json.dumps({"status": "unverified", "error": str(error)}, ensure_ascii=False),
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
