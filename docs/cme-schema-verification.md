# CME 官方生产协议核验记录

核查日期：2026-10-16。范围：CME Globex **MDP 3.0 Core SBE** 行情；不包含 iLink 订单接入、Streamlined SBE、TCP Session Management 或 New Release 测试环境。

## 核验结论

**2026-10-16 已取得官方 Production 目录的两份 XML，并确认它们与项目当前 schema 逐字节完全一致。无需替换 XML 或重新生成解码器。** 此前下载受阻的待办已解除：本次通过用户已登录的 Chrome 获取文件，用户随后将两份下载文件移入项目 `template/` 目录。

| 文件 | 用途 | 字节数 | 比对结果 |
|---|---|---|---|
| `template/templates_FixBinary.xml` | 官方生产目录的不带版本号文件 | 146935 | 相同 |
| `template/templates_v13.xml` | 官方生产目录的 v13 文件 | 146935 | 相同 |
| `schemas/cme/templates_v13.xml` | 项目实际生成解码器使用的固定副本 | 146935 | 相同 |

三份文件的校验值均为：

```text
MD5:    25913f3105137fa784f80d62f24c5fbb
SHA256: f45ebadf65ccb1a2d416f00ee755480072ecff030970cc170a78540541d78d60
```

除 MD5、SHA256 外，已直接比较完整字节内容，不仅比较文件名、版本号或文件大小。三份 XML 均为 `package=mktdata`、`schemaId=1`、`version=13`、`description=20230411`、`byteOrder=littleEndian`，包含 31 个消息模板。机器可读记录见 [schema 校验记录](../template/verification.json)。

核验范围是本次官方生产目录发布的 XML 与项目副本的一致性；不表示已经实现其中所有消息、通过现场行情验收，或核验了官方所有业务规则文档。实际能力仍以 [协议支持范围](protocol-support.md) 为准。

## 文件说明

`templates_FixBinary.xml` 是这里不带版本号的通用文件名；`templates_v13.xml` 的文件名显式标记 v13。下载前无法仅凭目录的修改时间判断两者是相同副本，还是通用文件已更新，所以同时下载进行交叉检查。本次确认二者内容完全相同，通用文件的实际版本也是 13；不据此假定其他环境或未来的同名文件始终相同。

这里的 `FixBinary` 指 FIX 消息的二进制编码模板。XML 描述字段类型、字段顺序/偏移、枚举、空值、重复组、blockLength 和 templateId 等规则，用于生成 SBE 字段访问器并校验消息布局。它本身是文本形式的协议定义，不是可执行解码库，也不是错误修复文件。运行时读取的是二进制行情，当前库使用预先生成的 C++ 代码，不会逐包加载或解析 XML。

项目继续只将 `schemas/cme/templates_v13.xml` 作为生成输入；`template/` 中的两份原文用于保留下载证据和后续核验，不需要同时参与编译。

## 已读取的官方资料

[官方 Production 模板目录](https://www.cmegroup.com/ftp/SBEFix/Production/Templates/) 在核查时列出：

| 文件 | 官方目录显示的修改时间 | 显示大小 |
|---|---|---|
| `templates_FixBinary.xml` | 2025-04-09 16:39 | 143K |
| `templates_v13.xml` | 2023-08-16 22:55 | 143K |

以上时间以本次 Chrome 页面显示为准。目录时间不是 schema 的语义版本；本次确认内容一致的依据是已下载原文的哈希和逐字节比较。两份文件虽显示不同修改时间，内容仍完全相同。

直接来源：[templates_FixBinary.xml](https://www.cmegroup.com/ftp/SBEFix/Production/Templates/templates_FixBinary.xml)、[templates_v13.xml](https://www.cmegroup.com/ftp/SBEFix/Production/Templates/templates_v13.xml)。

[官方 Message Schema 说明](https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457225459/MDP-30---Message-Schema) 描述 XML 中的类型、模板及版本演进规则。新增字段可能改变 blockLength、重复组布局和版本行为，所以仅检查 `version=13` 不足以判断内容一致。

[官方 FTP/SFTP 分发说明](https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457672149/MDP%2B3.0%2B-%2BFTP%2Band%2BSFTP%2BSite%2BInformation) 将环境和服务区分发布，推荐使用 SFTP；现行页面中的 SFTP 访问详情需要 CME Group 登录。应使用客户获授权的生产资料，不以历史页面中的账号信息或测试环境文件代替生产基线。

## 取得官方 XML 后如何完成比对

在项目根目录执行只读核对工具（Python 3 标准库，无额外依赖）：

```sh
# 联网核对官方 Production 别名文件；保存候选文件，不覆盖当前 schema。
python3 tools/check_cme_schema.py --save-candidate /tmp/cme-production-candidate.xml

# 本次通过 Chrome 下载的文件可直接复核。
python3 tools/check_cme_schema.py --official-file template/templates_FixBinary.xml
python3 tools/check_cme_schema.py --official-file template/templates_v13.xml
```

退出码 `0` 表示字节完全一致；`1` 表示内容不同、需要审阅差异；`2` 表示读取、下载或协议身份校验失败。工具同时显示双方 SHA256、版本和模板清单；离线模式明确标记来源未验证。文件不同可能只是排版或描述变化，不能直接推断线格式已经变化。下载失败不会回退到本地副本并报告通过。

若存在差异，先比较完整 XML，重点审阅类型长度、offset、presence/nullValue、常量与枚举、根块及组块长度、重复组维度、模板 ID 和 sinceVersion。确认官方生产环境与生效日期后，更新 XML、生成器约束及业务解码，再执行生成、C++11 编译、协议/恢复、PCAP、安装消费和 sanitizer 回归。最后记录官方文件哈希与核验结果。参见 [生成步骤](cme.md#更新-schema)。

## 协议与回传网络的边界

项目已移除供应商私有协议的实现、构建入口和支持说明。内置适配器仅处理明确支持的交易所官方编码，无法通过修改 `protocol` 或 `revision` 将私有行情转换为 CME MDP。

现场供应商可以转发交易所原始报文，组播目的地址、源 IP 和端口仍按现场配置；这不改变 SBE 消息布局。回传链路若重新编码，必须从上游取得官方原始格式后再接入本库。旧私有协议配置会在配置预检及打开客户端时返回 `unregistered protocol`，不会自动回退到官方解码器。
