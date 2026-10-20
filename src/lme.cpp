#include "internal.hpp"
#include <climits>
#include <cstdio>

namespace fqimpl {
namespace {
// 所有 LMEsource 业务通道共享 16 字节小端包头和 4 字节消息头。
// 构造时先检查整个包的消息边界，随后业务适配器可分别做字段校验和处理。
// Bytes 只借用输入 UDP payload；调用方不得在本次 feed 返回后保存其中指针。
class LmePacketView {
    Bytes bytes_;
    uint8_t count_;
    uint32_t first_;
    uint64_t send_;

  public:
    explicit LmePacketView(Bytes bytes) : bytes_(bytes), count_(0), first_(0), send_(0) {
        bytes_.require(0, 16);
        if (bytes_.u16(0) != bytes_.n)
            throw ParseError("LME PktSize mismatch");
        count_ = bytes_.u8(2);
        first_ = bytes_.u32(4);
        send_ = bytes_.u64(8);
        if (uint64_t(first_) + count_ > uint64_t(UINT32_MAX) + 1)
            throw ParseError("LME sequence overflow without reset");
        size_t offset = 16;
        for (unsigned i = 0; i < count_; ++i) {
            const uint16_t size = bytes_.u16(offset);
            if (size < 4)
                throw ParseError("invalid LME MsgSize");
            bytes_.sub(offset, size);
            offset += size;
        }
        if (offset != bytes_.n)
            throw ParseError("LME message count does not match packet length");
    }
    unsigned count() const { return count_; }
    uint32_t sequence() const { return first_; }
    uint64_t send_time() const { return send_; }
    template <class F> void each(F visit) const {
        size_t offset = 16;
        for (unsigned i = 0; i < count_; ++i) {
            Bytes message = bytes_.sub(offset, bytes_.u16(offset));
            visit(first_ + i, i, message, send_);
            offset += message.n;
        }
    }
};

// 乱序缓存和刷新暂存持有消息字节副本；不保存 UDP 输入缓冲区的借用指针。
// seq 在刷新暂存中保留原通道消息号，不能与其他通道的 seq 比较。
struct QueuedLmeMessage {
    std::vector<uint8_t> bytes;
    uint64_t send = 0, receive = 0;
    uint32_t flags = 0, index = 0, seq = 0;
};

// LBMA 307/807 参考拍卖价：405 为嵌套币种数据；实时序号与刷新序号分别维护。
// Client 串行驱动 feed/tick，暂存事件由本类持有直到完整 203 周期确认。
class LbmaAuctionPrices : public Adapter {
    History history_;
    uint64_t next_ = 0, refresh_next_ = 0, gap_started_ = 0, last_reset_time_ = 0;
    bool live_ = false, collecting_ = false, complete_ = false;
    uint32_t watermark_ = 0, dr_status_ = 0;
    uint64_t refresh_reset_time_ = 0, dr_time_ = 0;
    bool saw_heartbeat_ = false;
    uint32_t heartbeat_seq_ = 0;
    std::map<uint32_t, QueuedLmeMessage> pending_;
    std::vector<fq_event> staging_;
    void validate_message(Bytes m) {
        // MsgSize/MsgType 的公共边界已由 LmePacketView 检查。
        switch (m.u16(2)) {
        case 100:
        case 105:
        case 203:
            if (m.n != 8)
                throw ParseError("invalid LME control length");
            break;
        case 405: {
            m.require(0, 27);
            size_t p = 27;
            for (unsigned i = 0; i < m.u16(25); ++i) {
                m.require(p, 3);
                unsigned count = m.u16(p + 1);
                p += 3;
                if (count > (m.n - p) / 11)
                    throw ParseError("LME currency group exceeds message");
                for (unsigned j = 0; j < count; ++j, p += 11) {
                    uint64_t price = m.u64(p + 3);
                    if (price != UINT64_MAX && price > uint64_t(INT64_MAX))
                        throw ParseError("LME reference price exceeds int64 range");
                }
            }
            if (p != m.n)
                throw ParseError("LME 405 trailing bytes");
            break;
        }
        default:
            throw ParseError("unsupported message on LME LBMA profile");
        }
    }
    void references(Bytes m, bool snapshot) {
        size_t p = 27;
        for (unsigned i = 0; i < m.u16(25); ++i) {
            char kind = static_cast<char>(m.u8(p));
            unsigned count = m.u16(p + 1);
            p += 3;
            for (unsigned j = 0; j < count; ++j, p += 11) {
                fq_event e = base_;
                e.type = FQ_REFERENCE_PRICE;
                e.message_type = 405;
                e.event_time_ns = m.u64(4);
                e.data = {};
                fq_reference_price &r = e.data.reference_price;
                r.business_date = m.u32(13);
                copy_text(r.commodity, sizeof r.commodity, reinterpret_cast<const char *>(m.p + 17), 2);
                copy_text(r.session, sizeof r.session, reinterpret_cast<const char *>(m.p + 19), 2);
                copy_text(r.currency, sizeof r.currency, reinterpret_cast<const char *>(m.p + p), 3);
                r.price_type = kind;
                uint64_t price = m.u64(p + 3);
                r.price =
                    decimal(price == UINT64_MAX ? 0 : static_cast<int64_t>(price), -6, price != UINT64_MAX);
                r.bid_volume = m.u16(21);
                r.ask_volume = m.u16(23);
                if (!config_.commodities.empty() && !config_.commodities.count(r.commodity))
                    continue;
                if (snapshot) {
                    if (staging_.size() >= config_.max_instruments)
                        throw ParseError("LME refresh capacity exceeded");
                    staging_.push_back(e);
                } else {
                    e.flags |= FQ_EVENT_COMPLETE;
                    if (live_)
                        e.flags |= FQ_STREAM_CONTIGUOUS;
                    emit_(e);
                }
            }
        }
    }
    void try_complete() {
        if (!complete_)
            return;
        // An idle channel can be synchronized without any business messages.
        // Heartbeats report the previous sequence; they never consume a sequence.
        if (!next_ && saw_heartbeat_ &&
            (heartbeat_seq_ == watermark_ || (!watermark_ && heartbeat_seq_ == 1)))
            next_ = uint64_t(watermark_) + 1;
        if (!next_ || uint64_t(watermark_) + 1 > next_)
            return;
        if (!history_.empty() && uint64_t(watermark_) + 1 < history_.first()) {
            invalidate("LME refresh predates retained realtime messages");
            return;
        }
        uint64_t expected = uint64_t(watermark_) + 1;
        bool contiguous = true;
        history_.each([&](const History::Record &r, Bytes) {
            if (r.seq > watermark_) {
                if (r.seq != expected)
                    contiguous = false;
                expected = uint64_t(r.seq) + 1;
            }
        });
        if (!contiguous || expected != next_) {
            invalidate("LME refresh cannot bridge realtime gap");
            return;
        }
        live_ = true;
        complete_ = false;
        collecting_ = false;
        for (auto &e : staging_) {
            e.flags |= FQ_RECOVERED | FQ_EVENT_COMPLETE;
            emit_(e);
        }
        staging_.clear();
        fq_event saved = base_;
        history_.each([&](const History::Record &r, Bytes m) {
            if (r.seq > watermark_) {
                base_.sequence = r.seq;
                base_.receive_time_ns = r.received;
                base_.send_time_ns = r.sent;
                base_.message_index = r.index;
                base_.flags = (r.flags & FQ_REPLAYED) | FQ_RECOVERED;
                if (m.u16(2) == 405)
                    references(m, false);
            }
        });
        base_ = saved;
        status(FQ_OK, "LME LBMA refresh synchronized");
    }
    void accept(uint32_t seq, Bytes m, uint64_t send, uint64_t receive, uint32_t flags) {
        base_.sequence = seq;
        base_.send_time_ns = send;
        base_.receive_time_ns = receive;
        base_.flags = flags & FQ_REPLAYED;
        history_.push(seq, m, receive, send, flags, base_.message_index);
        next_ = uint64_t(seq) + 1;
        raw(m, m.u16(2), 413);
        if (m.u16(2) == 405 && live_)
            references(m, false);
    }
    void drain() {
        while (next_ <= UINT32_MAX) {
            auto it = pending_.find(static_cast<uint32_t>(next_));
            if (it == pending_.end())
                break;
            QueuedLmeMessage p = std::move(it->second);
            uint32_t seq = it->first;
            pending_.erase(it);
            base_.message_index = p.index;
            accept(seq, Bytes(p.bytes.data(), p.bytes.size()), p.send, p.receive, p.flags);
        }
        if (pending_.empty())
            gap_started_ = 0;
    }
    void realtime(uint32_t seq, Bytes m, const fq_datagram &d, uint64_t send) {
        if (!next_)
            next_ = seq;
        if (seq < next_)
            return;
        if (seq > next_) {
            if (pending_.count(seq))
                return;
            if (pending_.size() >= 128 || uint64_t(seq) - next_ > 128) {
                status(FQ_ECAPACITY, "LME reorder window exceeded", next_, seq);
                invalidate("LME gap requires refresh");
                pending_.clear();
                next_ = seq;
                gap_started_ = 0;
            } else {
                if (pending_.empty()) {
                    status(FQ_OK, "LME message gap: awaiting redundant line", next_, seq);
                    gap_started_ = d.receive_time_ns;
                }
                QueuedLmeMessage p;
                p.bytes.assign(m.p, m.p + m.n);
                p.send = send;
                p.receive = d.receive_time_ns;
                p.flags = d.flags;
                p.index = base_.message_index;
                pending_.emplace(seq, std::move(p));
                return;
            }
        }
        accept(seq, m, send, d.receive_time_ns, d.flags);
        drain();
    }

  public:
    LbmaAuctionPrices(const FeedConfig &c, Emit e) : Adapter(c, e), history_(c.cache_bytes) {
        staging_.reserve(c.max_instruments);
    }
    uint64_t capabilities() const override {
        return FQ_CAP_RAW | FQ_CAP_REFERENCE | FQ_CAP_ARBITRATION | FQ_CAP_SNAPSHOT;
    }
    void invalidate(const char *reason) override {
        live_ = false;
        collecting_ = false;
        complete_ = false;
        staging_.clear();
        history_.clear();
        saw_heartbeat_ = false;
        status(FQ_EPARSE, reason);
    }
    void tick(uint64_t time) override {
        if (!pending_.empty() && time >= gap_started_ && time - gap_started_ >= 2000000) {
            invalidate("LME gap timed out; waiting for full refresh cycle");
            next_ = pending_.begin()->first;
            drain();
            if (!pending_.empty())
                gap_started_ = time;
        }
    }
    void feed(const fq_datagram &d) override {
        prepare(d, FQ_LME);
        LmePacketView packet(Bytes(d.data, d.size));
        // 包边界由公共视图一次检查；业务字段仍须在任何回调前整包验证。
        packet.each([&](uint32_t, unsigned, Bytes m, uint64_t) { validate_message(m); });
        base_.send_time_ns = packet.send_time();
        base_.sequence = packet.sequence();
        tick(d.receive_time_ns);
        if (!packet.count()) {
            if (d.role == FQ_INCREMENTAL) {
                saw_heartbeat_ = true;
                heartbeat_seq_ = packet.sequence();
                try_complete();
            }
            return;
        }
        packet.each([&](uint32_t seq, unsigned index, Bytes m, uint64_t send) {
            const uint16_t type = m.u16(2);
            base_.message_type = type;
            base_.message_index = index;
            base_.sequence = seq;
            if (type == 105) {
                if (d.role != FQ_DR)
                    throw ParseError("DR signal must use dr endpoint role");
                uint32_t state = m.u32(4);
                if (state != 1 && state != 2)
                    throw ParseError("invalid LME DR status");
                if (send < dr_time_)
                    return;
                dr_time_ = send;
                if (state == dr_status_)
                    return;
                dr_time_ = send;
                dr_status_ = state;
                ++epoch_;
                base_.epoch = epoch_;
                invalidate(state == 1 ? "LME disaster recovery in progress"
                                      : "LME disaster recovery completed; awaiting refresh");
                pending_.clear();
                gap_started_ = 0;
                refresh_next_ = 0;
                next_ = 0;
                return;
            }
            if (type == 100) {
                if (d.role == FQ_SNAPSHOT) {
                    if (send <= refresh_reset_time_)
                        return;
                    refresh_reset_time_ = send;
                    refresh_next_ = m.u32(4);
                    collecting_ = false;
                    complete_ = false;
                    staging_.clear();
                    return;
                }
                if (d.role != FQ_INCREMENTAL)
                    throw ParseError("sequence reset on invalid LME role");
                if (send <= last_reset_time_)
                    return;
                last_reset_time_ = send;
                ++epoch_;
                base_.epoch = epoch_;
                invalidate("LME realtime sequence reset");
                pending_.clear();
                gap_started_ = 0;
                refresh_next_ = 0;
                dr_status_ = 0;
                next_ = m.u32(4);
                return;
            }
            if (dr_status_ == 1)
                return;
            if (d.role == FQ_INCREMENTAL && last_reset_time_ && send < last_reset_time_)
                return;
            if (d.role == FQ_SNAPSHOT && refresh_reset_time_ && send < refresh_reset_time_)
                return;
            if (d.role == FQ_INCREMENTAL) {
                if (type != 405)
                    throw ParseError("unexpected LME realtime control message");
                realtime(seq, m, d, send);
            } else if (d.role == FQ_SNAPSHOT) {
                if (live_)
                    return;
                const uint64_t msgseq = seq;
                if (refresh_next_ && msgseq < refresh_next_)
                    return;
                if (refresh_next_ && msgseq > refresh_next_) {
                    collecting_ = false;
                    complete_ = false;
                    staging_.clear();
                }
                refresh_next_ = msgseq + 1;
                if (type == 203) {
                    if (collecting_) {
                        watermark_ = m.u32(4);
                        complete_ = true;
                        collecting_ = false;
                        try_complete();
                    } else if (!complete_) {
                        collecting_ = true;
                        staging_.clear();
                    }
                } else if (type == 405 && collecting_)
                    references(m, true);
            } else
                throw ParseError("invalid LME endpoint role");
        });
        try_complete();
    }
};


// 只接收当前业务通道官方表列出的消息；新业务通道可增加枚举而无需修改公共拆包。
enum class CoreMessageProfile { OrderBook, TradeReplay };
void validate_core_message(Bytes m, CoreMessageProfile profile) {
    const uint16_t type = m.u16(2);
    size_t expected = 0;
    switch (type) {
    case 100: case 203: expected = 8; break;
    case 341:
        m.require(0, 63);
        expected = 63 + size_t(m.u16(57)) * 21;
        break;
    case 311: expected = 46; break;
    case 312: expected = 44; break;
    case 320: expected = 44; break;
    case 322:
        m.require(0, 23);
        expected = 23 + size_t(m.u8(22)) * 43;
        break;
    case 327: expected = 22; break;
    case 329: expected = 30; break;
    default: throw ParseError("unsupported LME Core L2 message type");
    }
    if (m.n != expected)
        throw ParseError("invalid LME Core L2 message length");
    if (profile == CoreMessageProfile::TradeReplay && type != 100 && type != 203 && type != 341)
        throw ParseError("unexpected message on LME trade replay channel");
    if (type == 322) {
        for (unsigned i = 0; i < m.u8(22); ++i) {
            size_t p = 23 + size_t(i) * 43;
            if ((m.u8(p + 40) != 'B' && m.u8(p + 40) != 'S') ||
                m.u8(p + 41) < 1 || m.u8(p + 41) > 15 || m.u8(p + 42) > 2)
                throw ParseError("invalid LME aggregate book side/level/action");
            if (m.u8(p + 42) != 2 && m.i64(p + 8) == INT64_MIN)
                throw ParseError("null LME book price on new/change");
        }
    }
    if (type == 341 && m.u8(32) > 1)
        throw ParseError("invalid LME trade cancellation flag");
    // 官方电子盘 341 的这三个扩展计数字段均为 0；非电子策略腿不属于这些 Core 流。
    if (type == 341 && (m.u16(55) || m.u16(57) || m.u32(59)))
        throw ParseError("unexpected LME electronic trade leg records");
    if (type == 341 || type == 311 || type == 312 || type == 320 || type == 322 ||
        type == 327 || type == 329) {
        const size_t venue = type == 341 ? 20 : 4;
        if (m.u8(venue) != 'E' || m.u8(venue + 1) != 'L')
            throw ParseError("LME Core channel requires Electronic venue");
    }
}

// 112 实时成交与 614 历史回放使用同一条官方 341 布局。
// 返回按值拥有的统一事件；来源、连续性和恢复标志由各业务适配器补充。
fq_event decode_electronic_trade(const fq_event &base, Bytes m) {
    fq_event e = base;
    e.type = FQ_TRADE;
    e.message_type = 341;
    e.instrument_id = m.u64(12);
    e.event_time_ns = m.u64(4);
    e.flags |= FQ_EVENT_COMPLETE;
    e.data = {};
    const int64_t price = m.i64(33);
    e.data.trade.price = decimal(price, -6, price != INT64_MIN);
    const uint32_t volume = m.u32(41);
    if (volume != UINT32_MAX) {
        e.data.trade.quantity = volume;
        e.data.trade.quantity_valid = 1;
    }
    e.data.trade.trade_id = m.u64(45);
    e.data.trade.trade_id_valid = e.data.trade.trade_id != UINT64_MAX;
    e.data.trade.action = m.u8(32); // TradeCancelFlag: 0=成交，1=取消。
    return e;
}

fq_level level(Bytes m, size_t p, bool implied) {
    fq_level out = {};
    out.price = decimal(m.i64(p + 8), -6, m.i64(p + 8) != INT64_MIN);
    uint64_t volume = m.u64(p + (implied ? 32 : 20));
    uint32_t orders = m.u32(p + (implied ? 28 : 16));
    if (volume != UINT64_MAX) {
        out.quantity = quantity(volume);
        out.quantity_valid = 1;
    }
    if (orders != UINT32_MAX) {
        if (orders > uint32_t(INT32_MAX))
            throw ParseError("LME order count exceeds int32 range");
        out.order_count = static_cast<int32_t>(orders);
        out.order_count_valid = 1;
    }
    return out;
}

// 322 的 PriceLevel 在总价位梯级中计数。显式/隐含侧保持相同的档位和价格，
// 各自数量、委托数取报文字段；即使一类数量为 0，也保留对齐档位，不将两者误当成独立排序。
void apply_book(Bytes m, Book &book, unsigned depth, bool snapshot) {
    for (unsigned i = 0; i < m.u8(22); ++i) {
        size_t p = 23 + size_t(i) * 43;
        unsigned side = m.u8(p + 40) == 'B' ? FQ_BID : FQ_ASK;
        unsigned position = m.u8(p + 41), action = m.u8(p + 42);
        if (position > depth)
            throw ParseError("LME price level exceeds configured depth");
        fq_level explicit_level = level(m, p, false);
        fq_level implied_level = level(m, p, true);
        if (snapshot && action == 0) {
            book.snapshot_level(side, position, explicit_level, depth);
            book.snapshot_level(side + 2, position, implied_level, depth);
        } else {
            book.update(side, position, action, explicit_level, depth);
            book.update(side + 2, position, action, implied_level, depth);
        }
    }
    book.time = m.u64(6);
}

// Core 112 实时 + 612 刷新：按合约维护四侧 L2 簿，并用 203 的实时水位衔接缓存。
// 112/612 序号空间独立；簿和暂存快照只由当前 Client 的串行 feed/tick 访问。
class CoreL2OrderBook : public Adapter {
    History history_;
    std::unordered_map<uint64_t, Book> books_, staged_;
    uint64_t next_ = 0, refresh_next_ = 0, gap_started_ = 0;
    uint64_t last_reset_time_ = 0, refresh_reset_time_ = 0;
    uint32_t watermark_ = 0, heartbeat_seq_ = 0;
    bool synced_ = false, collecting_ = false, complete_ = false, saw_heartbeat_ = false;
    std::map<uint32_t, QueuedLmeMessage> pending_;
    std::vector<QueuedLmeMessage> snapshot_events_;

    Book &book(std::unordered_map<uint64_t, Book> &which, uint64_t id) {
        auto it = which.find(id);
        if (it != which.end())
            return it->second;
        if (which.size() >= config_.max_instruments)
            throw ParseError("LME Core L2 instrument capacity exceeded");
        return which.emplace(id, Book()).first->second;
    }
    void delta(Bytes m) {
        uint64_t id = m.u64(14);
        if (!config_.instruments.empty() && !config_.instruments.count(id))
            return;
        for (unsigned i = 0; i < m.u8(22); ++i) {
            size_t p = 23 + size_t(i) * 43;
            unsigned side = m.u8(p + 40) == 'B' ? FQ_BID : FQ_ASK;
            for (unsigned implied = 0; implied < 2; ++implied) {
                fq_event e = base_;
                e.type = FQ_BOOK_DELTA;
                e.message_type = 322;
                e.instrument_id = id;
                e.event_time_ns = m.u64(6);
                e.flags |= FQ_EVENT_COMPLETE | (synced_ ? FQ_BOOK_VALID : 0) |
                           (contiguous_ ? FQ_STREAM_CONTIGUOUS : 0);
                e.data = {};
                e.data.delta.level = level(m, p, implied != 0);
                e.data.delta.position = m.u8(p + 41);
                e.data.delta.side = static_cast<uint8_t>(side + implied * 2);
                e.data.delta.action = m.u8(p + 42);
                emit_(e);
            }
        }
    }
    void trade(Bytes m) {
        uint64_t id = m.u64(12);
        if (!config_.instruments.empty() && !config_.instruments.count(id))
            return;
        fq_event e = decode_electronic_trade(base_, m);
        if (contiguous_)
            e.flags |= FQ_STREAM_CONTIGUOUS;
        emit_(e);
    }
    void trading_state(Bytes m) {
        if (m.u16(2) == 312 && !config_.instruments.empty() &&
            !config_.instruments.count(m.u64(14)))
            return;
        fq_event e = base_;
        e.type = FQ_TRADING_STATUS;
        e.message_type = m.u16(2);
        e.event_time_ns = m.u64(6);
        e.instrument_id = e.message_type == 312 ? m.u64(14) : 0;
        e.flags |= FQ_EVENT_COMPLETE;
        e.data = {};
        e.data.status.code = e.message_type == 312 ? m.u8(23) : m.u8(26);
        if (e.message_type == 311)
            copy_text(e.data.status.text, sizeof e.data.status.text,
                      reinterpret_cast<const char *>(m.p + 14), 12);
        else
            std::snprintf(e.data.status.text, sizeof e.data.status.text,
                          "LME instrument state condition=%c", char(m.u8(40)));
        emit_(e);
    }
    void indicative(Bytes m) {
        const uint64_t id = m.u64(14);
        if (!config_.instruments.empty() && !config_.instruments.count(id))
            return;
        fq_event e = base_;
        e.type = FQ_INDICATIVE;
        e.message_type = 320;
        e.instrument_id = id;
        e.event_time_ns = m.u64(6);
        e.flags |= FQ_EVENT_COMPLETE;
        e.data = {};
        const int64_t price = m.i64(22), mid = m.i64(34);
        e.data.indicative.price = decimal(price, -6, price != INT64_MIN);
        e.data.indicative.mid_price = decimal(mid, -6, mid != INT64_MIN);
        e.data.indicative.volume = m.u32(30);
        emit_(e);
    }
    void quote_request(Bytes m) {
        const uint64_t id = m.u64(14);
        if (!config_.instruments.empty() && !config_.instruments.count(id))
            return;
        fq_event e = base_;
        e.type = FQ_QUOTE_REQUEST;
        e.message_type = 329;
        e.instrument_id = id;
        e.event_time_ns = m.u64(6);
        e.flags |= FQ_EVENT_COMPLETE;
        e.data = {};
        e.data.quote_request.request_type = m.u8(22);
        e.data.quote_request.side = m.u8(23);
        e.data.quote_request.volume = m.u32(24);
        emit_(e);
    }
    void process(Bytes m, bool publish) {
        uint16_t type = m.u16(2);
        if (type == 322) {
            if (synced_) {
                uint64_t id = m.u64(14);
                Book &b = book(books_, id);
                apply_book(m, b, config_.depth, false);
                b.valid = true;
                if (publish)
                    publish_book(id, b);
            }
            if (publish)
                delta(m);
        } else if (type == 327) {
            if (synced_) {
                uint64_t id = m.u64(14);
                Book &b = book(books_, id);
                b.clear();
                b.valid = true; // 327 清盘后空盘口是已知状态，后续 322 再逐档建立。
                b.time = m.u64(6);
                if (publish)
                    publish_book(id, b);
            }
        } else if (publish && type == 341)
            trade(m);
        else if (publish && (type == 311 || type == 312))
            trading_state(m);
        else if (publish && type == 320)
            indicative(m);
        else if (publish && type == 329)
            quote_request(m);
    }
    void accept(uint32_t seq, Bytes m, uint64_t send, uint64_t receive, uint32_t flags, uint32_t index) {
        base_.sequence = seq;
        base_.message_index = index;
        base_.message_type = m.u16(2);
        base_.send_time_ns = send;
        base_.receive_time_ns = receive;
        base_.flags = flags & FQ_REPLAYED;
        history_.push(seq, m, receive, send, flags, index);
        next_ = uint64_t(seq) + 1;
        contiguous_ = synced_;
        raw(m, m.u16(2), 413);
        process(m, true);
        try_complete();
    }
    void drain() {
        while (next_ <= UINT32_MAX) {
            auto it = pending_.find(static_cast<uint32_t>(next_));
            if (it == pending_.end())
                break;
            QueuedLmeMessage p = std::move(it->second);
            uint32_t seq = it->first;
            pending_.erase(it);
            accept(seq, Bytes(p.bytes.data(), p.bytes.size()), p.send, p.receive, p.flags, p.index);
        }
        if (pending_.empty())
            gap_started_ = 0;
    }
    void realtime(uint32_t seq, Bytes m, const fq_datagram &d, uint64_t send, uint32_t index) {
        if (!next_)
            next_ = seq;
        if (seq < next_)
            return; // A/B 两线同一消息只处理一次。
        if (seq > next_) {
            if (pending_.count(seq))
                return;
            if (pending_.size() >= 128 || uint64_t(seq) - next_ > 128) {
                invalidate("LME Core L2 reorder window exceeded; waiting for refresh");
                pending_.clear();
                next_ = seq;
            } else {
                if (pending_.empty()) {
                    status(FQ_OK, "LME Core L2 gap: awaiting redundant line", next_, seq);
                    gap_started_ = d.receive_time_ns;
                }
                QueuedLmeMessage p;
                p.bytes.assign(m.p, m.p + m.n);
                p.send = send;
                p.receive = d.receive_time_ns;
                p.flags = d.flags;
                p.index = index;
                pending_.emplace(seq, std::move(p));
                return;
            }
        }
        accept(seq, m, send, d.receive_time_ns, d.flags, index);
        drain();
    }
    void try_complete() {
        if (!complete_)
            return;
        if ((!next_ || (next_ == 1 && history_.empty())) && saw_heartbeat_ && heartbeat_seq_ == watermark_)
            next_ = uint64_t(watermark_) + 1;
        if (!next_)
            return;
        if (uint64_t(watermark_) + 1 > next_) {
            // 612 的完整快照已经覆盖 112 缺口：旧实时缓存不再需要；
            // 保留水位之后的乱序消息，并从水位+1 开始按序补应用。
            history_.clear();
            pending_.erase(pending_.begin(), pending_.upper_bound(watermark_));
            next_ = uint64_t(watermark_) + 1;
            if (!pending_.empty())
                gap_started_ = base_.receive_time_ns;
            drain();
            if (!complete_)
                return;
        }
        if (!pending_.empty())
            return; // 已知水位之后仍有未补齐的缺口，不能宣称盘口有效。
        uint64_t expected = uint64_t(watermark_) + 1;
        bool contiguous = true;
        // Refresh 给出实时水位；只有缓存覆盖 [水位+1, 当前序号) 且完全连续才能发布有效盘口。
        history_.each([&](const History::Record &r, Bytes) {
            if (r.seq > watermark_) {
                if (r.seq != expected)
                    contiguous = false;
                expected = uint64_t(r.seq) + 1;
            }
        });
        if (!contiguous || expected != next_) {
            invalidate("LME Core L2 refresh cannot bridge realtime history");
            return;
        }
        for (auto &item : staged_)
            item.second.validate_snapshot();
        books_ = std::move(staged_);
        synced_ = true;
        contiguous_ = true;
        collecting_ = complete_ = false;
        fq_event saved = base_;
        history_.each([&](const History::Record &r, Bytes m) {
            if (r.seq > watermark_) {
                base_.sequence = r.seq;
                base_.send_time_ns = r.sent;
                base_.receive_time_ns = r.received;
                base_.message_index = r.index;
                base_.flags = (r.flags & FQ_REPLAYED) | FQ_RECOVERED;
                process(m, false);
            }
        });
        // 612 的非盘口状态在第二个 203 确认快照完整后再交付。
        for (const QueuedLmeMessage &entry : snapshot_events_) {
            Bytes m(entry.bytes.data(), entry.bytes.size());
            base_.sequence = entry.seq;
            base_.message_type = m.u16(2);
            base_.send_time_ns = entry.send;
            base_.receive_time_ns = entry.receive;
            base_.message_index = entry.index;
            base_.flags = (entry.flags & FQ_REPLAYED) | FQ_RECOVERED;
            process(m, true);
        }
        snapshot_events_.clear();
        base_ = saved;
        for (auto &item : books_) {
            item.second.valid = true;
            base_.sequence = watermark_; // 恢复视图的序号是 112 水位，不是 612 的包序号。
            base_.message_type = 322;
            publish_book(item.first, item.second, true);
        }
        base_ = saved;
        status(FQ_OK, "LME Core L2 refresh synchronized");
    }

  public:
    CoreL2OrderBook(const FeedConfig &c, Emit e) : Adapter(c, e), history_(c.cache_bytes) {}
    uint64_t capabilities() const override {
        return FQ_CAP_RAW | FQ_CAP_MBP | FQ_CAP_TRADES | FQ_CAP_SNAPSHOT | FQ_CAP_ARBITRATION;
    }
    void invalidate(const char *reason) override {
        synced_ = false;
        contiguous_ = false;
        collecting_ = complete_ = false;
        staged_.clear();
        snapshot_events_.clear();
        history_.clear();
        for (auto &item : books_)
            item.second.valid = false;
        status(FQ_EPARSE, reason);
    }
    void tick(uint64_t time) override {
        if (!pending_.empty() && time >= gap_started_ && time - gap_started_ >= 2000000) {
            uint32_t restart = pending_.begin()->first;
            invalidate("LME Core L2 gap timed out; waiting for refresh");
            next_ = restart;
            drain();
            if (!pending_.empty())
                gap_started_ = time;
        }
    }
    void feed(const fq_datagram &d) override {
        prepare(d, FQ_LME);
        LmePacketView packet(Bytes(d.data, d.size));
        tick(d.receive_time_ns);
        // 完整验证后才发任何业务事件，避免坏包前半段产生部分行情输出。
        packet.each([&](uint32_t, unsigned, Bytes m, uint64_t) {
            validate_core_message(m, CoreMessageProfile::OrderBook);
        });
        if (packet.count() == 0) {
            if (d.role == FQ_INCREMENTAL) {
                saw_heartbeat_ = true;
                heartbeat_seq_ = packet.sequence();
                try_complete();
            }
            return;
        }
        packet.each([&](uint32_t seq, unsigned index, Bytes m, uint64_t send) {
            uint16_t type = m.u16(2);
            base_.sequence = seq;
            base_.message_index = index;
            base_.send_time_ns = send;
            base_.message_type = type;
            if (type == 100) {
                if (d.role == FQ_INCREMENTAL) {
                    if (send <= last_reset_time_)
                        return;
                    last_reset_time_ = send;
                    ++epoch_;
                    base_.epoch = epoch_;
                    invalidate("LME Core L2 realtime sequence reset");
                    pending_.clear();
                    next_ = m.u32(4);
                } else if (d.role == FQ_SNAPSHOT) {
                    if (send <= refresh_reset_time_)
                        return;
                    refresh_reset_time_ = send;
                    refresh_next_ = m.u32(4);
                    collecting_ = complete_ = false;
                    staged_.clear();
                    snapshot_events_.clear();
                } else
                    throw ParseError("invalid LME Core L2 endpoint role");
                return;
            }
            if (d.role == FQ_INCREMENTAL) {
                if (last_reset_time_ && send < last_reset_time_)
                    return;
                if (type == 203)
                    throw ParseError("refresh marker on LME realtime channel");
                realtime(seq, m, d, send, index);
            } else if (d.role == FQ_SNAPSHOT) {
                if (refresh_reset_time_ && send < refresh_reset_time_)
                    return;
                if (refresh_next_ && seq < refresh_next_)
                    return;
                if (refresh_next_ && seq > refresh_next_) {
                    collecting_ = complete_ = false;
                    staged_.clear();
                    snapshot_events_.clear();
                }
                refresh_next_ = uint64_t(seq) + 1;
                if (type == 203) {
                    if (collecting_) {
                        watermark_ = m.u32(4);
                        complete_ = true;
                        collecting_ = false;
                        try_complete();
                    } else if (!complete_) {
                        collecting_ = true;
                        staged_.clear();
                        snapshot_events_.clear();
                    }
                } else if (collecting_ && type == 322) {
                    uint64_t id = m.u64(14);
                    apply_book(m, book(staged_, id), config_.depth, true);
                } else if (collecting_ && type == 327)
                    book(staged_, m.u64(14)).clear();
                else if (collecting_ && (type == 311 || type == 312 || type == 320 || type == 329)) {
                    if (snapshot_events_.size() >= size_t(config_.max_instruments) * 4)
                        throw ParseError("LME Core L2 snapshot status capacity exceeded");
                    QueuedLmeMessage entry;
                    entry.bytes.assign(m.p, m.p + m.n);
                    entry.send = send;
                    entry.receive = d.receive_time_ns;
                    entry.flags = d.flags;
                    entry.index = index;
                    entry.seq = seq;
                    snapshot_events_.push_back(std::move(entry));
                }
                if (config_.raw)
                    raw(m, type, 413);
            } else
                throw ParseError("invalid LME Core L2 endpoint role");
        });
    }
};

// Core 614 仅处理独立的成交回放周期；不参与 112/612 的盘口同步。
// 每轮可能重放相同 MatchID，交给消费方按业务键去重。
class CoreTradeReplay : public Adapter {
    uint64_t next_ = 0, last_reset_time_ = 0;
    bool collecting_ = false, contiguous_cycle_ = false;
    void trade(Bytes m) {
        uint64_t id = m.u64(12);
        if (!config_.instruments.empty() && !config_.instruments.count(id))
            return;
        fq_event e = decode_electronic_trade(base_, m);
        e.flags |= FQ_RECOVERED;
        if (contiguous_cycle_)
            e.flags |= FQ_STREAM_CONTIGUOUS;
        emit_(e);
    }
  public:
    CoreTradeReplay(const FeedConfig &c, Emit e) : Adapter(c, e) {}
    uint64_t capabilities() const override {
        return FQ_CAP_RAW | FQ_CAP_TRADES | FQ_CAP_SNAPSHOT | FQ_CAP_ARBITRATION;
    }
    void invalidate(const char *reason) override {
        collecting_ = contiguous_cycle_ = false;
        next_ = 0;
        status(FQ_EPARSE, reason);
    }
    void feed(const fq_datagram &d) override {
        if (d.role != FQ_SNAPSHOT)
            throw ParseError("LME 614 requires snapshot endpoint role");
        prepare(d, FQ_LME);
        LmePacketView packet(Bytes(d.data, d.size));
        packet.each([&](uint32_t, unsigned, Bytes m, uint64_t) {
            validate_core_message(m, CoreMessageProfile::TradeReplay);
        });
        packet.each([&](uint32_t seq, unsigned index, Bytes m, uint64_t send) {
            uint16_t type = m.u16(2);
            base_.sequence = seq;
            base_.message_index = index;
            base_.message_type = type;
            base_.send_time_ns = send;
            if (type == 100) {
                if (send <= last_reset_time_)
                    return; // 两条冗余线的同一 Sequence Reset 只处理一次。
                last_reset_time_ = send;
                ++epoch_;
                base_.epoch = epoch_;
                collecting_ = false;
                next_ = m.u32(4);
                contiguous_cycle_ = true;
                return;
            }
            if (next_ && seq < next_)
                return; // A/B 线重复报文。
            if (last_reset_time_ && send < last_reset_time_)
                return;
            if (next_ && seq > next_) {
                contiguous_cycle_ = false;
                status(FQ_EPARSE, "LME 614 replay gap", next_, seq);
            }
            next_ = uint64_t(seq) + 1;
            if (type == 203) {
                if (collecting_) {
                    collecting_ = false;
                    status(FQ_OK, "LME 614 trade replay cycle complete");
                } else {
                    collecting_ = true;
                    contiguous_cycle_ = true;
                }
                return;
            }
            raw(m, type, 413);
            if (collecting_ && type == 341)
                trade(m);
        });
    }
};

} // namespace

// 协议入口只按业务通道选择实现；612 是 112 的刷新端点，不单独创建适配器。
std::unique_ptr<Adapter> make_lmesource_adapter(const FeedConfig &c, Emit e) {
    switch (c.channel) {
    case 307: return std::unique_ptr<Adapter>(new LbmaAuctionPrices(c, e));
    case 112: return std::unique_ptr<Adapter>(new CoreL2OrderBook(c, e));
    case 614: return std::unique_ptr<Adapter>(new CoreTradeReplay(c, e));
    default: throw std::runtime_error("unsupported LME logical channel");
    }
}
} // namespace fqimpl
