# futures_quote

> **仅供测试环境的行情接入、解码与接口联调，请勿用于生产环境。** 项目重点是组播接入和解码；完整订单簿、断线重连后的历史补全及严格快照加增量恢复尚无完整保证，需要使用方自行实现和验证。

**完整阅读入口：[CME / LME 行情接入与解码指南](docs/market-data-guide.zh-CN.md)。** 建议先按其中的准备、版本核对、配置和 Client 两种驱动方式章节操作；模块实现细节再按指南链接阅读。

C++11 统一行情 SDK：同一套 client 可用于程序内接入、动态库/静态库分发和命令行行情打印。

当前实现为 **0.1.0 首批测试版本**：CME MDP 3.0 schema v13 的 MBP 与相关消息、LMEsource v4.13 的 LBMA 拍卖参考价，以及 LME Core 112/612 聚合盘口和 614 成交回放。LME 官网在 2026-10-20 列出的当前规范为 v4.18，其中旧版 405 拍卖价消息已移除；项目没有完成新版适配，必须核对测试环境实际协议版本。仅以交易所官方发布的协议定义编码与解码规则，不提供供应商私有协议适配。

2026-10-16 已下载 CME 官方 Production 目录的两份 schema，与项目 v13 副本逐字节完全一致，无需更新 XML；详见 [核验记录](docs/cme-schema-verification.md)。

完整能力及限制见 [协议支持表](docs/protocol-support.md)。本版本不包含 CME 完整 MBO、LME 参考数据/合约代码映射或 TCP 补包。现有版本依据附件和固定 schema 实现，**尚未用生产网络或真实录包验收**。

## 1. 编译与离线运行

需要 CMake ≥ 3.16、C++11 编译器和 POSIX 平台。macOS 可本地开发；Linux 为部署目标，Linux CI 配置已提供。依赖源码和 CME 生成头文件均已包含，普通构建无需 Java、联网或额外安装 yaml-cpp。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure

python3 tools/make_demo.py build/demo.pcap
./build/fq_dump --config config/replay.yaml --pcap build/demo.pcap --rate 0
```

演示数据为明确构造的测试样本，不是交易所历史行情。预期输出：9 个数据包、2 次完整 CME 盘口、9 条 LME 多币种参考价，`parse_errors=0`。其中包括 VLAN、IPv4 options、CME 跨包事件和 LME A/B 部分重叠。

构建产物：

| 产物 | 用途 |
|---|---|
| `libfutures_quote.so` / macOS `.dylib` | 动态库，导出 `fq_*` C ABI |
| `libfutures_quote.a` | 静态库，包含私有 YAML 实现 |
| `fq_dump` | 配置检查、UDP 行情打印、PCAP 回放 |
| `fq_bench` | 合成 CME 成交报文的进程内基准 |
| `fq_c_example` / `fq_cpp_example` | 分别验证 C 动态链接、C++ 静态链接 |

`-DFQ_BUILD_APPS=OFF`、`-DFQ_BUILD_TESTS=OFF` 可关闭工具或测试。`-DFQ_SANITIZE=ON` 启用 ASan/UBSan；不要用该构建测延时。

## 2. 配置组播并收行情

```sh
cp config/example.yaml config/local.yaml
# 编辑 local.yaml 中 network.interface_ip，填写行情网卡的本机 IPv4
./build/fq_dump --config config/local.yaml --check-config
./build/fq_dump --config config/local.yaml --duration 30 --rate 1000
```

配置模板已填写附件中的 CME 384 铂钯、LME 307/807 及 Core 112/612/614 地址。**现场地址与配置一致、回传内容保持受支持的交易所原生编码时，客户只需修改本机 IP**；若供应商调整组播地址、源地址或端口，则修改对应 endpoint。`source_ip` 存在时使用源指定组播；删除该项则使用 ASM。无需另外运行 `join_multicast`，也不使用混杂模式或原始网卡抓包。

`--check-config` 校验字段、范围、协议版本和适配器，不能证明网络已经连通。实际启动还会检查 IP 是否属于本机、socket 和加组是否成功。配置及排障详见 [配置与运行](docs/configuration.md)。

CentOS 7 / GCC 4.8.5 无外网服务器联调 382、384 通道，使用 [离线部署步骤](docs/offline-centos7.md) 和 [双通道配置模板](config/cme-382-384-test.yaml)。本地执行 `./scripts/package.sh output`，上传生成的压缩包、校验文件和 `unpack.sh`；服务器执行 `./unpack.sh`、`./scripts/build.sh`、`./scripts/run.sh`。

`--rate` 在回调入队前按 feed 限制每秒进入打印队列的普通事件数，不减少 SDK 的解析和回调；状态和缺口始终尝试打印。默认 100，`--rate 0` 打印全部事件。若打印队列仍耗尽，会计数并继续接收，丢失的只是终端打印样本。生产程序应直接接回调，不依赖终端文本。

## 3. 在项目中使用

```sh
cmake --install build --prefix "$PWD/build/install"
```

调用方的 CMake：

```cmake
find_package(futures_quote CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE futures_quote::shared)
# 或 futures_quote::static
```

配置调用方时使用 `-DCMAKE_PREFIX_PATH=/path/to/futures_quote/build/install`。动态库仍依赖系统 C++ 运行库；静态库也需要使用 C++ 链接器和线程库。“提供 .a”不意味着整个可执行文件完全静态化。

```cpp
#include <futures_quote/client.hpp>

static void on_event(void*, const fq_event* e) {
    if (e->type == FQ_BOOK && (e->flags & FQ_BOOK_VALID)) {
        // e->data.book.sides[FQ_BID][0]：买一（先检查 counts[FQ_BID]）
        // 在此处理或复制。所有指针仅在本次回调期间有效。
    }
}

int main() {
    fq::Client client(on_event);
    client.open("config/local.yaml");
    client.start();
    // 业务线程运行；退出时：
    client.request_stop();
    client.join();
}
```

也可以不用库内线程，由调用方循环 `client.poll(2)`；自有收包框架用 `client.feed(datagram)` 输入 **UDP payload**。`client.replay(path)` 直接回放 PCAP。它们共享解析和恢复代码，但同一个 client 不能混用并发驱动方式。

C 接口见 [fq.h](include/futures_quote/fq.h)，C++ 包装见 [client.hpp](include/futures_quote/client.hpp)，线程、状态、价格及错误契约见 [API 文档](docs/api.md)。安装后独立项目验证命令：

```sh
./tools/test_install.sh "$PWD/build"
```

## 4. 模块说明

- [总指南](docs/market-data-guide.zh-CN.md)：测试准备、官网版本、CME/LME 接入、Client 用法和排障。
- [架构与流程](docs/architecture.md)：Client、Transport、Adapter 的职责、数据与错误流程。
- [CME 编码、解码和恢复](docs/cme.md)：包头、SBE、重复组、定点价格、事件边界和快照水位。
- [LME 编码、解码和恢复](docs/lme.md)：消息序号、A/B 重叠、405 嵌套币种、RFS 和 reset。
- [架构与新交易所接入](docs/extending.md)：插件边界、接口示例、扩展验收。
- [依赖来源](THIRD_PARTY.md)：版本、生成步骤和校验和。
