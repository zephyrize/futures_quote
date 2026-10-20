# 依赖与协议来源

本项目新增实现与第三方内容分开存放。此文件记录来源，不改变交易所文档/数据或第三方代码的原有权利。

| 内容 | 固定版本 / 来源 | 校验与许可 |
|---|---|---|
| yaml-cpp 源码 | [jbeder/yaml-cpp 0.8.0](https://github.com/jbeder/yaml-cpp/releases/tag/0.8.0) | MIT，保留 `third_party/yaml-cpp-0.8.0/LICENSE` |
| SBE 代码生成器 | [aeron-io/simple-binary-encoding 1.35.0](https://github.com/aeron-io/simple-binary-encoding/tree/1.35.0) | Apache-2.0，见 `third_party/SBE-LICENSE`；生成器不作为运行依赖 |
| CME v13 schema | 本地副本最初来自 `cme_data_feed` 提交 b65c7a6；权威更新入口为 [CME Production](https://www.cmegroup.com/ftp/SBEFix/Production/Templates/) | schema id 1 / version 13；2026-10-16 与官方 Production 两份下载文件逐字节一致，见 [核验记录](docs/cme-schema-verification.md)。保留交易所原有权利，不重新授权为本项目代码 |
| CME 生成头文件 | SBE 1.35.0 + 上述固定 schema | `generated/cme/fq_sbe/`，namespace=fq_sbe；保留可复现步骤 |
| CME 结构校验器 | 项目 `tools/generate_layout.py` | 从同一 XML 生成 `generated/cme/cme_layout.hpp` |
| LME 布局依据 | 附件官方 Client Interface Specification v4.13 / Developer Guide v1.1 | 手工按文档实现，文档原件不复制进发行包 |

SHA256：

```text
yaml-cpp-0.8.0.tar.gz
fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16
sbe-all-1.35.0.jar
d11f4bb7c71322e3538613b2da18b11625be400465b29a09db80bc9785cea3ab
schemas/cme/templates_v13.xml
f45ebadf65ccb1a2d416f00ee755480072ecff030970cc170a78540541d78d60
```

SBE jar 来源为 Maven Central `uk.co.real-logic:sbe-all:1.35.0`。需要重新生成时使用 JRE/JDK 17+（本机使用 Temurin 21），设置 `SBE_JAR`、可选 `JAVA_CMD`，运行 `tools/generate_sbe.sh`。普通 C++ 编译无需 Java。此项目未采用 sbepp、OnixS、B2BITS 或其他示例项目源码。
