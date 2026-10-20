# CME 官方下载原文

2026-10-16 从 CME 官方 `SBEFix/Production/Templates/` 通过 Chrome 获取，用户移入本目录。两份 XML 均保留下载内容，未手工修改。

- `templates_FixBinary.xml`：不带版本号的通用文件名，本次内容为 schema 1/version 13。
- `templates_v13.xml`：带版本号的文件，本次与前者以及 `schemas/cme/templates_v13.xml` 逐字节相同。
- `verification.json`：官方 URL、核验日期、文件大小、MD5 和 SHA256。

构建生成输入仍为 `schemas/cme/templates_v13.xml`。完整结论、文件用途和后续核验步骤见 [核验记录](../docs/cme-schema-verification.md)。
