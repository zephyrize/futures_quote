#ifndef FQ_INTERNAL_HPP
#define FQ_INTERNAL_HPP
#include "futures_quote/fq.h"
#include "futures_quote/adapter.h"
#include "bytes.hpp"
#include <array>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include <functional>
#include <chrono>
#include <algorithm>
namespace fqimpl {
inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}
// 物理接收端点；同一逻辑 feed 可包含 A/B 两线和增量、快照、定义等多种角色。
struct Endpoint {
    std::string group, source;
    uint16_t port;
    uint32_t role, line;
};
// 启动时解析出的逻辑 feed 配置；热路径读取结构化字段，不逐包解析 YAML。
// 同一 feed 的多端点共用一个 Adapter，序号和恢复规则由该协议决定。
struct FeedConfig {
    uint32_t id = 0, channel = 0, depth = 10, max_instruments = 4096;
    size_t cache_bytes = 16 * 1024 * 1024;
    std::string protocol, revision, adapter_config;
    bool raw = false, publish_invalid = false;
    std::vector<Endpoint> endpoints;
    std::set<uint64_t> instruments;
    std::set<std::string> symbols, groups, commodities;
};
struct Config {
    std::string interface_ip = "0.0.0.0";
    int receive_buffer = 16 * 1024 * 1024;
    uint32_t batch = 16;
    std::vector<FeedConfig> feeds;
};
Config load_config(const std::string &path);
typedef std::function<void(const fq_event &)> Emit;
inline fq_decimal decimal(int64_t m, int8_t exponent, bool valid = true) {
    fq_decimal p = {};
    p.mantissa = m;
    p.exponent = exponent;
    p.valid = valid ? 1 : 0;
    return p;
}
// 库内四侧 MBP 状态，使用固定容量数组避免每次插入/删除档位时分配内存。
// valid=盘口可用，metadata=定义就绪，dirty=本事件内有变更，pending=恢复尚待完成。
// rpt 是合约更新序号，watermark 是已应用快照对应的增量包水位，二者不能混用。
struct Book {
    std::array<std::array<fq_level, FQ_MAX_DEPTH>, 4> levels;
    std::array<uint32_t, 4> counts;
    fq_instrument definition;
    uint32_t rpt = 0, watermark = 0;
    uint64_t time = 0;
    bool valid = false, metadata = false, dirty = false, pending = false;
    Book() : levels(), counts(), definition() {}
    // 清空价量和更新状态，但保留合约定义；水位/元数据是否重置由协议场景决定。
    void clear() {
        levels = {};
        counts = {};
        valid = false;
        dirty = false;
        pending = false;
        rpt = 0;
    }
    // 输入档位为 1 基；非法操作直接报错，禁止通过自动补空档掩盖丢包或错误恢复。
    void update(unsigned side, unsigned position, unsigned action, const fq_level &value, unsigned depth) {
        if (side >= 4 || position == 0 || position > depth || depth > FQ_MAX_DEPTH || action > 5)
            throw ParseError("invalid book side/level/action");
        const unsigned i = position - 1;
        auto &a = levels[side];
        auto &n = counts[side];
        if (action == 0) {
            if (i > n)
                throw ParseError("book insert leaves a level gap");
            for (unsigned j = std::min(n, depth - 1); j > i; --j)
                a[j] = a[j - 1];
            a[i] = value;
            n = std::min(n + 1, depth);
        } else if (action == 1) {
            if (i >= n)
                throw ParseError("book change missing level");
            a[i] = value;
        } else if (action == 2) {
            if (i >= n)
                throw ParseError("book delete missing level");
            for (unsigned j = i; j + 1 < n; ++j)
                a[j] = a[j + 1];
            a[--n] = {};
        } else if (action == 3) {
            if (i >= n)
                throw ParseError("book delete-through missing level");
            for (unsigned j = 0; j + position < n; ++j)
                a[j] = a[j + position];
            unsigned old = n;
            n -= position;
            for (unsigned j = n; j < old; ++j)
                a[j] = {};
        } else if (action == 4) {
            if (i >= n)
                throw ParseError("book delete-from missing level");
            for (unsigned j = i; j < n; ++j)
                a[j] = {};
            n = i;
        } else {
            if (i > n)
                throw ParseError("book overlay leaves gap");
            a[i] = value;
            n = std::max(n, position);
        }
        dirty = true;
    }
    void snapshot_level(unsigned side, unsigned position, const fq_level &value, unsigned depth) {
        if (side >= 4 || !position || position > depth)
            throw ParseError("invalid snapshot level");
        if (levels[side][position - 1].price.valid)
            throw ParseError("duplicate snapshot level");
        levels[side][position - 1] = value;
        counts[side] = std::max(counts[side], position);
    }
    void validate_snapshot() const {
        for (unsigned s = 0; s < 4; ++s)
            for (unsigned i = 0; i < counts[s]; ++i)
                if (!levels[s][i].price.valid)
                    throw ParseError("snapshot contains level gap");
    }
};
/* 有限恢复环：vector 在初始化时一次分配固定槽位，push 复制数据，不保存输入缓冲区指针。
 * 每槽 16 KiB；CME 存一个包，LME 存一条消息，容量表示条数上限而不是固定保留时长。
 * 满后覆盖最旧记录；协议层必须检查水位仍在覆盖范围内，不能假设旧数据始终存在。
 * each 按写入顺序提供借用视图，遍历期间不可修改该 History。
 */
class History {
  public:
    static const size_t slot_size = 16384;
    struct Record {
        uint32_t seq = 0;
        size_t size = 0;
        uint64_t received = 0, sent = 0;
        uint32_t flags = 0, index = 0;
    };

  private:
    std::vector<uint8_t> bytes_;
    std::vector<Record> records_;
    size_t begin_ = 0, count_ = 0;

  public:
    explicit History(size_t budget) : bytes_((budget / slot_size) * slot_size), records_(budget / slot_size) {
        if (records_.empty())
            throw std::runtime_error("recovery buffer too small");
    }
    void clear() { begin_ = count_ = 0; }
    void push(uint32_t seq, Bytes data, uint64_t received, uint64_t sent = 0, uint32_t flags = 0,
              uint32_t message_index = 0) {
        if (data.n > slot_size)
            throw ParseError("datagram exceeds recovery slot limit (16384)");
        size_t index = (begin_ + count_) % records_.size();
        if (count_ == records_.size())
            begin_ = (begin_ + 1) % records_.size();
        else
            ++count_;
        records_[index].seq = seq;
        records_[index].size = data.n;
        records_[index].received = received;
        records_[index].sent = sent;
        records_[index].flags = flags;
        records_[index].index = message_index;
        std::memcpy(&bytes_[index * slot_size], data.p, data.n);
    }
    uint32_t first() const { return count_ ? records_[begin_].seq : 0; }
    uint32_t last() const { return count_ ? records_[(begin_ + count_ - 1) % records_.size()].seq : 0; }
    bool empty() const { return count_ == 0; }
    template <class F> void each(F f) const {
        for (size_t i = 0; i < count_; ++i) {
            size_t k = (begin_ + i) % records_.size();
            f(records_[k], Bytes(&bytes_[k * slot_size], records_[k].size));
        }
    }
};
// 协议层公共接口：负责把 payload 转成统一事件；序号、A/B、恢复细节由派生类实现。
// Client 串行调用 feed/tick，因此内部状态不自带锁；emit_ 必须同步使用借用事件。
class Adapter {
  protected:
    FeedConfig config_;
    Emit emit_;
    fq_event base_ = {};
    uint64_t epoch_ = 1;
    bool contiguous_ = false;
    void prepare(const fq_datagram &d, uint32_t exchange) {
        base_ = {};
        base_.struct_size = sizeof(base_);
        base_.abi_version = FQ_ABI_VERSION;
        base_.feed_id = config_.id;
        base_.channel_id = config_.channel;
        base_.exchange = exchange;
        base_.epoch = epoch_;
        base_.receive_time_ns = d.receive_time_ns;
        base_.flags = d.flags & FQ_REPLAYED;
    }
    void status(int code, const char *message, uint64_t expected = 0, uint64_t received = 0) {
        fq_event e = base_;
        e.type = expected || received ? FQ_GAP : FQ_STATUS;
        e.data = {};
        e.data.status.code = code;
        e.data.status.expected = expected;
        e.data.status.received = received;
        copy_text(e.data.status.text, sizeof e.data.status.text, message, std::strlen(message));
        emit_(e);
    }
    void raw(Bytes b, uint32_t type, uint32_t revision) {
        if (!config_.raw)
            return;
        fq_event e = base_;
        e.type = FQ_RAW_MESSAGE;
        e.message_type = type;
        e.data = {};
        e.data.raw.data = b.p;
        e.data.raw.size = static_cast<uint32_t>(b.n);
        e.data.raw.schema_version = revision;
        emit_(e);
    }
    bool selected(uint64_t id, const Book &b) const {
        return (config_.instruments.empty() || config_.instruments.count(id)) &&
               (config_.symbols.empty() || (b.metadata && config_.symbols.count(b.definition.symbol))) &&
               (config_.groups.empty() || (b.metadata && config_.groups.count(b.definition.security_group)));
    }
    uint32_t flags(const Book &b) const {
        return base_.flags | (b.valid ? FQ_BOOK_VALID : 0) | (b.metadata ? FQ_METADATA_READY : 0) |
               (contiguous_ ? FQ_STREAM_CONTIGUOUS : 0);
    }
    // 仅在协议层确认事件边界后调用；默认过滤无效盘口，发布时借用 Book 内的数组。
    void publish_book(uint64_t id, Book &b, bool recovered = false) {
        if (!selected(id, b) || (!b.valid && !config_.publish_invalid))
            return;
        fq_event e = base_;
        e.type = FQ_BOOK;
        e.instrument_id = id;
        e.flags = flags(b) | FQ_EVENT_COMPLETE | (recovered ? FQ_RECOVERED : 0);
        e.event_time_ns = b.time;
        e.report_sequence = b.rpt;
        e.data = {};
        e.data.book.depth = config_.depth;
        for (unsigned s = 0; s < 4; ++s) {
            e.data.book.sides[s] = b.levels[s].data();
            e.data.book.counts[s] = b.counts[s];
        }
        emit_(e);
        b.dirty = false;
    }

  public:
    Adapter(const FeedConfig &config, Emit emit) : config_(config), emit_(emit) {}
    virtual ~Adapter() {}
    virtual void feed(const fq_datagram &) = 0;
    virtual uint64_t capabilities() const = 0;
    // 坏包或传输缺口后撤销“有效状态”，并通知消费者；具体恢复办法由协议负责。
    virtual void invalidate(const char *reason) = 0;
    // Unix 纳秒时间，用于无需新报文也能触发的乱序等待超时等维护。
    virtual void tick(uint64_t) {}
};
std::unique_ptr<Adapter> make_cme(const FeedConfig &, Emit);
// LMEsource v4.13 业务工厂：307=LBMA 参考价，112=Core L2，614=成交回放。
// 返回对象独占所有权；未知通道抛异常，由 Client 转为配置错误。
std::unique_ptr<Adapter> make_lmesource_adapter(const FeedConfig &, Emit);
std::unique_ptr<Adapter> make_adapter(const FeedConfig &, Emit);
// 传输层向 Client 交付的轻量视图。data 是 UDP payload，生命周期仅覆盖本次接收回调。
// 在线和 PCAP 路由都产生此结构，time 分别来自用户态接收时钟或录包时间。
struct Received {
    uint32_t feed, role, line;
    const uint8_t *data;
    size_t size;
    uint64_t time;
};
// 纯同步的 UDP 接收器：构造时校验网卡并加组，poll 时收包，析构时关闭 socket。
// Impl 隐藏平台 socket 细节；线程、业务状态和回调异常处理由 Client 管理。
class Receiver {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit Receiver(const Config &);
    ~Receiver();
    void poll(int timeout, const std::function<void(const Received &)> &callback, uint64_t &drops,
              uint64_t &truncations);
};
void replay_pcap(const Config &, const std::string &, const std::function<void(const Received &)> &,
                 const std::function<bool()> &);
} // namespace fqimpl
#endif
