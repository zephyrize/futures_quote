#ifndef FUTURES_QUOTE_FQ_H
#define FUTURES_QUOTE_FQ_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32) && defined(FQ_SHARED)
#ifdef FQ_BUILD
#define FQ_API __declspec(dllexport)
#else
#define FQ_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define FQ_API __attribute__((visibility("default")))
#else
#define FQ_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* 公共 C ABI：只暴露定长字段和不透明句柄，不暴露 STL 或协议生成器类型。
 * 入参结构先清零，再填写 struct_size=sizeof(结构)、abi_version=FQ_ABI_VERSION。
 * reserved 字段保留为 0；改变结构布局或字段语义时必须考虑 ABI 兼容性。
 * 字段的完整消费规则见 docs/api.md。 */
#define FQ_ABI_VERSION 1u
#define FQ_MAX_DEPTH 64u /* 统一 MBP 视图每侧最多保留的档数。 */
/* 一个句柄拥有独立的配置、协议状态、接收器和可选工作线程；由 create/destroy 管理。 */
typedef struct fq_client fq_client;
/* 统一错误码；具体失败原因用 fq_last_error 获取，不能只凭错误码推断协议细节。 */
enum fq_result {
    FQ_OK = 0,            /* 成功 */
    FQ_EINVAL = -1,       /* 参数、配置或 ABI 校验失败 */
    FQ_ESTATE = -2,       /* 当前生命周期状态不允许操作，如客户端正被其他驱动入口使用 */
    FQ_EIO = -3,          /* 网络、文件或工作线程 I/O 操作失败 */
    FQ_EPARSE = -4,       /* 报文格式或业务语义解析失败 */
    FQ_EUNSUPPORTED = -5, /* 接口预留的不支持能力错误码；实际返回值以各入口为准 */
    FQ_ECAPACITY = -6,    /* 资源或容量不足 */
    FQ_ECALLBACK = -7     /* 用户回调抛出异常或在错误通知期间失败 */
};
/* type 决定 fq_event.data 中可读取的 union 成员，不可同时解释为多种事件。 */
enum fq_event_type {
    FQ_STATUS = 1,          /* 运行/恢复状态通知，使用 data.status */
    FQ_GAP = 2,             /* 序号缺口，data.status 中包含预期和实际序号 */
    FQ_INSTRUMENT = 3,      /* 合约公共属性及新增/修改/删除动作 */
    FQ_BOOK = 4,            /* 完整 MBP 盘口视图，消费前检查 FQ_BOOK_VALID */
    FQ_BOOK_DELTA = 5,      /* 单条盘口更新，可能位于尚未结束的交易所事件中 */
    FQ_TRADE = 6,           /* 成交；CME 当前为成交汇总，不保证是单个订单撮合 */
    FQ_STATISTIC = 7,       /* 成交量、结算价、限价等统计项目 */
    FQ_REFERENCE_PRICE = 8, /* 参考价，如 LME 拍卖价格，不代表连续买卖盘口 */
    FQ_RAW_MESSAGE = 9,     /* 协议原始消息的临时字节视图 */
    FQ_ORDER = 10,          /* 逐笔委托事件，不等于完整 MBO 已恢复 */
    FQ_TRADING_STATUS = 11, /* 交易状态，data.status.code 保留协议状态值 */
    FQ_INDICATIVE = 12,     /* LME 320 开盘指示价/中间价及指示量 */
    FQ_QUOTE_REQUEST = 13  /* LME 329 询价请求 */
};
enum fq_exchange { FQ_CME = 1, FQ_LME = 2, FQ_CUSTOM = 100 };
/* 端点用途：INCREMENTAL=实时增量，SNAPSHOT=快照/刷新，DEFINITION=合约定义，DR=灾备信号。
 * 同一 feed 的不同角色可共享端口，由目的组播地址区分。 */
enum fq_role { FQ_INCREMENTAL = 0, FQ_SNAPSHOT = 1, FQ_DEFINITION = 2, FQ_DR = 3 };
/* 独立位标志，应使用按位与判断；盘口有效、历史连续和事件完成不是同一件事。 */
enum fq_flags {
    FQ_BOOK_VALID = 1,        /* 当前盘口状态可用，不保证此后不会发生缺口或变陈旧 */
    FQ_METADATA_READY = 2,    /* 合约定义已就绪，可使用 symbol/group 等映射 */
    FQ_STREAM_CONTIGUOUS = 4, /* 适配器所定义的当前序列连续，不承诺缺失历史成交已补全 */
    FQ_RECOVERED = 8,         /* 本事件来自快照恢复或缓存补应用 */
    FQ_REPLAYED = 16,         /* 输入来自离线回放或由调用方标记为回放 */
    FQ_EVENT_COMPLETE = 32    /* 完整事件输出；CME 盘口以 EndOfEvent 为发布边界 */
};
/* 同时用作 fq_book.sides/counts 的数组下标，显式盘与隐含盘分别存放。 */
enum fq_side { FQ_BID = 0, FQ_ASK = 1, FQ_IMPLIED_BID = 2, FQ_IMPLIED_ASK = 3 };
/* 适配器能力位，使用 fq_capabilities 查询，不代表当前已经完成行情同步。
 * ORDER_EVENTS 只代表可发布委托事件，MBO 才代表完整逐笔订单簿能力。
 * 枚举中定义能力不代表每个内置协议均已实现该能力。 */
enum fq_capability {
    FQ_CAP_RAW = 1,
    FQ_CAP_MBP = 2,
    FQ_CAP_TRADES = 4,
    FQ_CAP_REFERENCE = 8,
    FQ_CAP_SNAPSHOT = 16,
    FQ_CAP_ARBITRATION = 32,
    FQ_CAP_ORDER_EVENTS = 64,
    FQ_CAP_MBO = 128,
    FQ_CAP_TCP_RECOVERY = 256
};
/* 精确定点数：真实值 = mantissa × 10^exponent，valid=0 时数值不可使用。
 * 价格可能为负；空值不能转换为价格 0。CME 常见 -9，LME 405 为 -6。 */
typedef struct fq_decimal {
    int64_t mantissa;
    int8_t exponent;
    uint8_t valid;
    uint8_t reserved[6];
} fq_decimal;
/* 单档价量。quantity 与 order_count 的有效位独立，未知委托数不应当作 0。
 * 数量单位沿用源协议（通常为合约/手），不统一换算为金额或实物重量。 */
typedef struct fq_level {
    fq_decimal price;
    int64_t quantity;
    int32_t order_count;
    uint8_t quantity_valid, order_count_valid;
    uint8_t reserved[2];
} fq_level;
/* 完整盘口的借用视图：每侧只有 [0, counts[side]) 有效，0 下标为该侧第一档。
 * depth 是容量/配置深度，不等于实际档数。指针仅在本次事件回调内有效。 */
typedef struct fq_book {
    const fq_level *sides[4];
    uint32_t counts[4];
    uint32_t depth;
} fq_book;
/* 盘口增量：position 为 1 基档位，side 取 fq_side。
 * action：0 新增、1 修改、2 删除、3 删除至本档、4 从本档起删除、5 覆盖、6 清空盘口。
 * 删除/清空时不能假设 level 中的价格和数量有业务意义。 */
typedef struct fq_delta {
    fq_level level;
    uint32_t position;
    uint8_t side, action;
    uint8_t reserved[2];
} fq_delta;
/* 成交价格、数量和可选成交标识；trade_id_valid=0 表示源消息没有有效 ID。
 * aggressor/action 保留适配器映射的协议值；CME aggressor 1=买方、2=卖方。
 * 无 aggressor 或 order_count 的适配器保留零值，不据此推断撮合方向或委托数。 */
typedef struct fq_trade {
    fq_decimal price;
    int64_t quantity;
    uint64_t trade_id;
    uint32_t order_count;
    uint8_t aggressor, action, quantity_valid, trade_id_valid;
} fq_trade;
/* 合约公共元数据。字符串由内置适配器补 NUL 结尾；CME action 为 A/M/D。
 * tick_size 为最小价格变动；display_factor 独立保存，SDK 不自动乘入行情价格。
 * 可变 tick 的合约可能没有有效 tick_size，不能用一个空值代表固定 tick。 */
typedef struct fq_instrument {
    char symbol[64], security_group[16], currency[4];
    uint32_t depth;
    fq_decimal tick_size, display_factor;
    uint8_t action;
    uint8_t reserved[7];
} fq_instrument;
/* LME LBMA 参考价：business_date 为 YYYYMMDD，commodity 为 PT/PD，session 为 AM/PM。
 * price_type 为 D（日价）或 M（月均价），currency 为 USD/EUR/GBP；这些字段共同标识条目。
 * bid_volume/ask_volume 是拍卖买卖兴趣量，不是盘口买一/卖一数量。 */
typedef struct fq_reference_price {
    fq_decimal price;
    uint32_t business_date;
    char commodity[3], session[3], currency[4];
    char price_type;
    uint8_t reserved;
    uint16_t bid_volume, ask_volume;
} fq_reference_price;
/* LME 320：price 与 mid_price 互斥；均按 -6 定点且分别携带空值有效位。
 * volume 为预估开盘量，0 表示无预估量，不据此推断成交。 */
typedef struct fq_indicative {
    fq_decimal price, mid_price;
    uint32_t volume;
} fq_indicative;
/* LME 329：request_type 1=单次手动、2=自动连续；side 为 B/S，0 表示双边。
 * volume 是原报文 Amended delta quantity，不解释为盘口数量。 */
typedef struct fq_quote_request {
    uint32_t volume;
    uint8_t request_type, side;
    uint8_t reserved[2];
} fq_quote_request;
/* 统计项目：kind 通常为 CME MDEntryType 字符值，1000/1001/1002 为高限/低限/波动范围。
 * CME 49 的 reference_date 是 epoch 起的天数（65535 为空），不是 YYYYMMDD；
 * attributes 在 49 中是 SettlPriceType 位图，在 51 中是 OpenCloseSettlFlag。
 * 应结合 event.message_type 解释属性；quantity_valid 与 price.valid 分别判断。 */
typedef struct fq_statistic {
    fq_decimal price;
    int64_t quantity;
    uint32_t reference_date;
    uint32_t kind;
    uint8_t quantity_valid, action, attributes, reserved;
} fq_statistic;
/* 委托更新：priority/quantity 各自有有效位。action 保留源协议更新动作。
 * reference_id 是 CME 46 第二组到第一组的消息内关联索引，不是永久合约 ID。
 * 此事件本身不能证明已经拥有该合约的全部存量订单。 */
typedef struct fq_order {
    fq_decimal price;
    uint64_t order_id, priority;
    int64_t quantity;
    uint8_t side, action, quantity_valid, priority_valid;
    uint32_t reference_id;
} fq_order;
/* STATUS/GAP 使用 code、text 描述诊断，GAP 的 expected/received 为缺口边界。
 * TRADING_STATUS 复用该结构，但 code 是交易所状态值，不应解释为 fq_result。 */
typedef struct fq_status {
    int32_t code;
    char text[160];
    uint64_t expected, received;
} fq_status;
/* 借用原始字节，仅回调期间有效；size 为字节数。
 * CME 包含 MsgSize+SBE 头，不含 12 字节包头；LME 包含 MsgSize/MsgType。
 * schema_version 当前 CME=13、LME=413，并非各协议共享的版本编码标准。 */
typedef struct fq_raw {
    const uint8_t *data;
    uint32_t size;
    uint32_t schema_version;
} fq_raw;
/* 统一事件信封：type 选择 data 的成员，message_type 保留交易所模板/消息类型。
 * feed_id 来自配置，channel_id 是交易所逻辑通道；instrument_id 不是全市场唯一键。
 * CME ID 为 SecurityID；LME 405 的 ID 为 0。
 * epoch 在适配器重置/灾备切换时递增，序号只在同一会话和同一序列空间内比较。
 * sequence：CME 为包序号，LME 为消息序号；LME 刷新条目与实时条目使用不同序列空间。
 * report_sequence 为 CME 合约级 RptSeq，message_index 为包内 0 基索引。
 * 三个时间均为 Unix 纳秒：事件时间、源发送时间、本机接收/回放时间；未知字段为 0。
 * receive_time_ns 不是硬件时间戳；恢复事件时间的来源详见 docs/api.md。 */
typedef struct fq_event {
    uint32_t struct_size, abi_version, type, exchange;
    uint32_t feed_id, channel_id, flags, message_type;
    uint64_t epoch, instrument_id, sequence, event_time_ns, send_time_ns, receive_time_ns;
    uint32_t report_sequence, message_index;
    union {
        fq_book book;
        fq_delta delta;
        fq_trade trade;
        fq_instrument instrument;
        fq_reference_price reference_price;
        fq_indicative indicative;
        fq_quote_request quote_request;
        fq_statistic statistic;
        fq_order order;
        fq_status status;
        fq_raw raw;
    } data;
} fq_event;
/* 同步回调：start 模式在库工作线程执行，poll/feed/replay 模式在调用线程执行。
 * event 和其中所有指针仅借用到回调返回；异步队列必须深复制 book/raw 的数据。
 * 不得阻塞、抛异常、destroy/join/poll 或递归 feed 当前 client；允许 request_stop。
 * 库捕获回调异常后返回 FQ_ECALLBACK 并请求停止，不能靠跨 C 边界异常控制业务。 */
typedef void (*fq_on_event)(void *user, const fq_event *event);
/* 回调表被 create 按值复制；user 指向的业务上下文由调用方保证存活到回调全部结束。 */
typedef struct fq_callbacks {
    uint32_t struct_size, abi_version;
    fq_on_event on_event;
    void *user;
} fq_callbacks;
/* 外部收包入口：data/size 只包含 UDP payload，不带 Ethernet/IP/UDP 头。
 * 数据在 fq_feed 返回前保持有效；feed_id 选择适配器，role 选择增量/快照等处理路径。
 * receive_time_ns 使用与其他输入相同的 Unix 纳秒时钟，用于排序等待与恢复超时。
 * flags 当前读取 FQ_REPLAYED；line 为 0(A)/1(B)，两线共享逻辑序号空间。 */
typedef struct fq_datagram {
    uint32_t struct_size, abi_version, feed_id, role;
    const uint8_t *data;
    size_t size;
    uint64_t receive_time_ns;
    uint32_t flags, line; /* line 用于线路标记，不创建独立序号状态。 */
} fq_datagram;
/* client 生命周期累计计数，重新 open 不清零；events 包含 raw/status 等所有事件。
 * socket_drops 当前由 Linux 内核溢出计数提供，其他平台为 0 不代表一定没丢包。
 * 字段逐个原子读取，不保证所有计数来自完全相同的瞬间。 */
typedef struct fq_statistics {
    uint32_t struct_size, abi_version;
    uint64_t datagrams, bytes, events, parse_errors, socket_drops, truncated_datagrams;
} fq_statistics;
/* 返回库内静态版本字符串，调用方不得释放。 */
FQ_API const char *fq_version(void);
/* 创建句柄，callbacks 可为空；成功后由调用方负责 destroy，失败时 *out 为 NULL。 */
FQ_API int fq_create(const fq_callbacks *callbacks, fq_client **out);
/* 请求停止、等待工作线程并释放资源；允许 NULL。不得在回调内调用，亦不得与其他 API 并发销毁。 */
FQ_API void fq_destroy(fq_client *client);
/* 加载配置并创建适配器/缓存，不加入组播；失败保留旧配置。运行中需先 stop/join。 */
FQ_API int fq_open(fq_client *client, const char *config_file);
/* 建立组播接收器并启动单个工作线程；成功仅代表本地启动成功，不保证行情已同步。 */
FQ_API int fq_start(fq_client *client);
/* 仅设置停止请求，不等待退出；可从其他线程或回调中调用，已开始的处理可能继续发出回调。 */
FQ_API void fq_request_stop(fq_client *client);
/* 等待工作线程退出并关闭接收器，返回线程结果；通常先 request_stop，禁止回调内 join。 */
FQ_API int fq_join(fq_client *client);
/* poll/feed/replay 由单一调用者驱动，与 start 模式互斥，同一 client 不可递归或并发驱动。 */
/* 调用线程执行一次收包/超时处理，timeout_ms 范围 0–1000；首次调用按需创建接收器。 */
FQ_API int fq_poll(fq_client *client, int timeout_ms);
/* 同步处理一份 payload，并在返回前完成相应回调；负值表示输入或解析失败。 */
FQ_API int fq_feed(fq_client *client, const fq_datagram *datagram);
/* 按文件顺序尽快回放经典 PCAP，不模拟原时间间隔；首个解析错误即停止，事件带 REPLAYED。 */
FQ_API int fq_replay_pcap(fq_client *client, const char *path);
/* 读取累计统计；out 需预填 struct_size/abi_version，可与工作线程并发查询。 */
FQ_API int fq_get_statistics(fq_client *client, fq_statistics *out);
/* 查询 feed 的能力位；未知 feed、无效句柄或锁繁忙时返回 0，建议启动前查询。 */
FQ_API uint64_t fq_capabilities(fq_client *client, uint32_t feed_id);
/* 拷贝最近的错误诊断；容量允许时保证 NUL 结尾，返回完整文本所需字节数（含 NUL）。
 * buffer=NULL 或 capacity=0 可仅查询长度；成功操作不保证清除之前的诊断。 */
FQ_API size_t fq_last_error(fq_client *client, char *buffer, size_t capacity);
/* 检查配置并尝试构造适配器，不加入组播；诊断写入 error，不能据此判断网络连通。 */
FQ_API int fq_check_config(const char *path, char *error, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
