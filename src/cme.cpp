#include "internal.hpp"
#include "cme_layout.hpp"
#include "fq_sbe/MDIncrementalRefreshBook46.h"
#include "fq_sbe/MDIncrementalRefreshTradeSummary48.h"
#include "fq_sbe/MDIncrementalRefreshVolume37.h"
#include "fq_sbe/MDIncrementalRefreshDailyStatistics49.h"
#include "fq_sbe/MDIncrementalRefreshSessionStatistics51.h"
#include "fq_sbe/MDIncrementalRefreshLimitsBanding50.h"
#include "fq_sbe/SnapshotFullRefresh52.h"
#include "fq_sbe/MDInstrumentDefinitionFuture54.h"
#include "fq_sbe/MDInstrumentDefinitionOption55.h"
#include "fq_sbe/MDInstrumentDefinitionSpread56.h"
#include "fq_sbe/MDIncrementalRefreshOrderBook47.h"
#include "fq_sbe/SecurityStatus30.h"
namespace fqimpl {
namespace {
template <class T> T wrap(Bytes b) {
    T m;
    m.wrapForDecode(reinterpret_cast<char *>(const_cast<uint8_t *>(b.p)), 10, b.u16(2), 13, b.n);
    return m;
}
unsigned side(char c) {
    switch (c) {
    case '0':
    case 'x':
        return 0;
    case '1':
    case 'w':
        return 1;
    case 'E':
        return 2;
    case 'F':
        return 3;
    default:
        throw ParseError("unsupported CME book entry type");
    }
}
fq_level level(int64_t price, int32_t qty, int32_t orders) {
    fq_level l = {};
    l.price = decimal(price, -9, price != INT64_MAX);
    l.quantity = qty;
    l.quantity_valid = qty != INT32_MAX;
    l.order_count = orders;
    l.order_count_valid = orders != INT32_MAX;
    if (l.quantity_valid && qty < 0)
        throw ParseError("negative CME book quantity");
    return l;
}
class Cme : public Adapter {
    std::unordered_map<uint64_t, Book> books_;
    History history_;
    uint64_t next_ = 0, last_reset_time_ = 0, gap_started_ = 0;
    std::array<uint64_t, 4> other_next_ = {{0, 0, 0, 0}};
    std::array<uint64_t, 4> other_time_ = {{0, 0, 0, 0}};
    struct Pending {
        std::vector<uint8_t> bytes;
        uint64_t time;
        uint32_t flags;
    };
    std::map<uint32_t, Pending> pending_;
    bool replaying_ = false;
    uint64_t replay_id_ = 0;
    std::vector<uint64_t> dirty_;
    Book &book(uint64_t id) {
        auto it = books_.find(id);
        if (it != books_.end())
            return it->second;
        if (books_.size() >= config_.max_instruments)
            throw ParseError("instrument capacity exceeded");
        return books_.emplace(id, Book()).first->second;
    }
    void mark(uint64_t id, Book &b) {
        if (!b.dirty && !replaying_)
            dirty_.push_back(id);
        b.dirty = true;
    }
    bool wanted(uint64_t id) const { return !replaying_ || id == replay_id_; }
    void finish() {
        if (replaying_)
            return;
        for (uint64_t id : dirty_) {
            Book &b = books_.at(id);
            const bool recovered = b.pending;
            if (b.pending && base_.sequence >= b.watermark) {
                b.pending = false;
                b.valid = b.metadata;
            }
            publish_book(id, b, recovered);
            b.dirty = false;
        }
        dirty_.clear();
    }
    void book_message(Bytes b) {
        auto m = wrap<fq_sbe::MDIncrementalRefreshBook46>(b);
        base_.event_time_ns = m.transactTime();
        auto &entries = m.noMDEntries();
        struct Ref {
            uint64_t id;
            fq_decimal price;
            uint8_t side;
        };
        std::array<Ref, 256> refs = {};
        unsigned ri = 0;
        while (entries.hasNext()) {
            auto &x = entries.next();
            uint64_t id = static_cast<uint32_t>(x.securityID());
            char ty = x.mDEntryTypeRaw();
            unsigned s = ty == 'J' ? 0 : side(ty);
            fq_level l = level(x.mDEntryPx().mantissa(), x.mDEntrySize(), x.numberOfOrders());
            refs[ri++] = {id, l.price, static_cast<uint8_t>(s)};
            if (!wanted(id))
                continue;
            Book &bk = book(id);
            if (ty == 'w' || ty == 'x') {
                if (!replaying_ && selected(id, bk)) {
                    fq_event e = base_;
                    e.type = FQ_STATISTIC;
                    e.instrument_id = id;
                    e.report_sequence = x.rptSeq();
                    e.data = {};
                    e.data.statistic.kind = static_cast<uint8_t>(ty);
                    e.data.statistic.price = l.price;
                    e.data.statistic.quantity = l.quantity;
                    e.data.statistic.quantity_valid = l.quantity_valid;
                    e.data.statistic.action = x.mDUpdateActionRaw();
                    emit_(e);
                }
                continue;
            }
            if (base_.sequence <= bk.watermark)
                continue;
            if (x.rptSeq() <= bk.rpt && bk.rpt)
                continue;
            bool was_dirty = bk.dirty;
            if (ty == 'J') {
                bk.clear();
                bk.valid = bk.metadata;
                bk.watermark = static_cast<uint32_t>(base_.sequence - 1);
            }
            if (ty != 'J' && (bk.valid || bk.pending || replaying_)) {
                if (x.mDUpdateActionRaw() != 2 && x.mDUpdateActionRaw() != 3 && x.mDUpdateActionRaw() != 4 &&
                    !l.price.valid)
                    throw ParseError("null CME book price");
                bk.update(s, x.mDPriceLevel(), x.mDUpdateActionRaw(), l, config_.depth);
            }
            bk.rpt = x.rptSeq();
            bk.time = m.transactTime();
            bk.dirty = was_dirty;
            mark(id, bk);
            if (!replaying_ && selected(id, bk)) {
                fq_event e = base_;
                e.type = FQ_BOOK_DELTA;
                e.instrument_id = id;
                e.report_sequence = bk.rpt;
                e.flags = flags(bk);
                e.data = {};
                e.data.delta.level = l;
                e.data.delta.position = x.mDPriceLevel();
                e.data.delta.side = static_cast<uint8_t>(s);
                e.data.delta.action = ty == 'J' ? 6 : x.mDUpdateActionRaw();
                emit_(e);
            }
        }
        auto &orders = m.noOrderIDEntries();
        while (orders.hasNext()) {
            auto &x = orders.next();
            if (replaying_)
                continue;
            uint8_t ref = x.referenceID();
            // A null reference cannot be resolved without a full MBO order index.
            if (ref == 255)
                continue;
            if (ref == 0 || ref > ri)
                throw ParseError("CME order reference outside MD entries");
            const Ref &r = refs[ref - 1];
            Book &bk = book(r.id);
            if (!selected(r.id, bk))
                continue;
            fq_event e = base_;
            e.type = FQ_ORDER;
            e.instrument_id = r.id;
            e.flags = flags(bk) & ~FQ_BOOK_VALID;
            e.data = {};
            e.data.order.order_id = x.orderID();
            e.data.order.priority = x.mDOrderPriority();
            e.data.order.priority_valid = x.mDOrderPriority() != UINT64_MAX;
            e.data.order.price = r.price;
            e.data.order.quantity = x.mDDisplayQty();
            e.data.order.quantity_valid = x.mDDisplayQty() != INT32_MAX;
            e.data.order.side = r.side;
            e.data.order.action = x.orderUpdateActionRaw();
            e.data.order.reference_id = ref;
            emit_(e);
        }
        if (m.matchEventIndicator().endOfEvent())
            finish();
    }
    template <class M> void definition(Bytes b) {
        auto m = wrap<M>(b);
        uint64_t id = static_cast<uint32_t>(m.securityID());
        Book &bk = book(id);
        fq_instrument def = {};
        m.getSymbol(def.symbol, 20);
        m.getSecurityGroup(def.security_group, 6);
        m.getCurrency(def.currency, 3);
        copy_text(def.symbol, sizeof def.symbol, def.symbol, 20);
        copy_text(def.security_group, sizeof def.security_group, def.security_group, 6);
        def.action = static_cast<uint8_t>(m.securityUpdateActionRaw());
        def.tick_size =
            decimal(m.minPriceIncrement().mantissa(), -9, m.minPriceIncrement().mantissa() != INT64_MAX);
        def.display_factor = decimal(m.displayFactor().mantissa(), -9);
        auto &events = m.noEvents();
        while (events.hasNext())
            events.next();
        auto &feeds = m.noMDFeedTypes();
        while (feeds.hasNext()) {
            auto &f = feeds.next();
            char name[4] = {};
            f.getMDFeedType(name, 3);
            if (std::memcmp(name, "GBX", 3) == 0 && f.marketDepth() > 0)
                def.depth = static_cast<uint32_t>(f.marketDepth());
        }
        auto &attrs = m.noInstAttrib();
        while (attrs.hasNext())
            attrs.next();
        auto &lots = m.noLotTypeRules();
        while (lots.hasNext())
            lots.next();
        if (def.depth > config_.depth)
            throw ParseError("configured depth smaller than instrument market depth");
        bk.definition = def;
        bk.metadata = def.action != 'D';
        if (!bk.metadata)
            bk.clear();
        fq_event e = base_;
        e.type = FQ_INSTRUMENT;
        e.instrument_id = id;
        e.data = {};
        e.data.instrument = def;
        e.flags = flags(bk);
        if (selected(id, bk))
            emit_(e);
    }
    void snapshot(Bytes bytes) {
        auto m = wrap<fq_sbe::SnapshotFullRefresh52>(bytes);
        uint64_t id = static_cast<uint32_t>(m.securityID());
        Book &old = book(id);
        if (old.valid)
            return;
        const uint32_t water = m.lastMsgSeqNumProcessed();
        if (!history_.empty() && uint64_t(water) + 1 < history_.first()) {
            status(FQ_ECAPACITY, "snapshot predates retained incrementals");
            return;
        }
        if (history_.empty() || water >= next_)
            return; // Need live data through snapshot boundary.
        Book fresh;
        fresh.definition = old.definition;
        fresh.metadata = old.metadata;
        fresh.rpt = m.rptSeq();
        fresh.watermark = water;
        fresh.time = m.transactTime();
        auto &entries = m.noMDEntries();
        while (entries.hasNext()) {
            auto &x = entries.next();
            char type = x.mDEntryTypeRaw();
            if (type == '0' || type == '1' || type == 'E' || type == 'F') {
                fq_level l = level(x.mDEntryPx().mantissa(), x.mDEntrySize(), x.numberOfOrders());
                if (!l.price.valid)
                    throw ParseError("null CME snapshot price");
                fresh.snapshot_level(side(type), static_cast<unsigned>(x.mDPriceLevel()), l, config_.depth);
            }
        }
        fresh.validate_snapshot();
        dirty_.erase(std::remove(dirty_.begin(), dirty_.end(), id), dirty_.end());
        old = fresh;
        old.pending = true;
        fq_event saved = base_;
        replaying_ = true;
        replay_id_ = id;
        try {
            history_.each([&](const History::Record &rec, Bytes packet) {
                if (rec.seq > water) {
                    base_.sequence = rec.seq;
                    decode_packet(packet, FQ_INCREMENTAL);
                }
            });
        } catch (...) {
            replaying_ = false;
            old.valid = false;
            old.pending = false;
            base_ = saved;
            throw;
        }
        replaying_ = false;
        base_ = saved;
        old.pending = false;
        old.valid = old.metadata;
        old.dirty = false;
        // A snapshot is event-consistent; cached tail must also end an event.
        if (event_open_) {
            old.pending = true;
            old.valid = false;
            mark(id, old);
        } else {
            base_.sequence = next_ - 1;
            publish_book(id, old, true);
            base_ = saved;
        }
    }
    bool event_open_ = false;
    static void statistic_details(fq_sbe::MDIncrementalRefreshDailyStatistics49::NoMDEntries &x,
                                  fq_statistic &s) {
        s.reference_date = x.tradingReferenceDate();
        s.attributes = x.settlPriceType().rawValue();
    }
    static void statistic_details(fq_sbe::MDIncrementalRefreshSessionStatistics51::NoMDEntries &x,
                                  fq_statistic &s) {
        s.attributes = x.openCloseSettlFlagRaw();
    }
    template <class M> void statistic_group(Bytes b, uint32_t kind) {
        auto m = wrap<M>(b);
        base_.event_time_ns = m.transactTime();
        auto &entries = m.noMDEntries();
        while (entries.hasNext()) {
            auto &x = entries.next();
            uint64_t id = static_cast<uint32_t>(x.securityID());
            Book &bk = book(id);
            if (!selected(id, bk))
                continue;
            fq_event e = base_;
            e.type = FQ_STATISTIC;
            e.instrument_id = id;
            e.report_sequence = x.rptSeq();
            e.flags = flags(bk);
            e.data = {};
            e.data.statistic.kind = kind ? kind : static_cast<uint8_t>(x.mDEntryTypeRaw());
            e.data.statistic.price =
                decimal(x.mDEntryPx().mantissa(), -9, x.mDEntryPx().mantissa() != INT64_MAX);
            e.data.statistic.quantity = x.mDEntrySize();
            e.data.statistic.quantity_valid = x.mDEntrySize() != INT32_MAX;
            e.data.statistic.action = x.mDUpdateActionRaw();
            statistic_details(x, e.data.statistic);
            emit_(e);
        }
        if (m.matchEventIndicator().endOfEvent())
            finish();
    }
    void message(Bytes b, uint32_t role) {
        uint16_t type = b.u16(4);
        base_.message_type = type;
        if (replaying_ && type != 46)
            return;
        if (!replaying_)
            raw(b, type, 13);
        if (type == 12)
            return;
        if (type == 54) {
            definition<fq_sbe::MDInstrumentDefinitionFuture54>(b);
            return;
        }
        if (type == 55) {
            definition<fq_sbe::MDInstrumentDefinitionOption55>(b);
            return;
        }
        if (type == 56) {
            definition<fq_sbe::MDInstrumentDefinitionSpread56>(b);
            return;
        }
        if (type == 52) {
            if (role != FQ_SNAPSHOT)
                throw ParseError("CME snapshot on wrong role");
            snapshot(b);
            return;
        }
        if (type == 4)
            return; // Reset handled before sequence arbitration.
        if (role != FQ_INCREMENTAL)
            throw ParseError("unsupported message on CME recovery/definition feed");
        if (type == 46) {
            book_message(b);
            return;
        }
        if (type == 48) {
            auto m = wrap<fq_sbe::MDIncrementalRefreshTradeSummary48>(b);
            base_.event_time_ns = m.transactTime();
            auto &entries = m.noMDEntries();
            while (entries.hasNext()) {
                auto &x = entries.next();
                uint64_t id = static_cast<uint32_t>(x.securityID());
                Book &bk = book(id);
                if (!selected(id, bk))
                    continue;
                fq_event e = base_;
                e.type = FQ_TRADE;
                e.instrument_id = id;
                e.report_sequence = x.rptSeq();
                e.flags = flags(bk);
                e.data = {};
                e.data.trade.price = decimal(x.mDEntryPx().mantissa(), -9);
                e.data.trade.quantity = x.mDEntrySize();
                e.data.trade.quantity_valid = 1;
                e.data.trade.order_count = static_cast<uint32_t>(x.numberOfOrders());
                e.data.trade.trade_id = x.mDTradeEntryID();
                e.data.trade.trade_id_valid = x.mDTradeEntryID() != UINT32_MAX;
                e.data.trade.aggressor = x.aggressorSideRaw();
                e.data.trade.action = x.mDUpdateActionRaw();
                emit_(e);
            }
            if (m.matchEventIndicator().endOfEvent())
                finish();
            return;
        }
        if (type == 37) {
            auto m = wrap<fq_sbe::MDIncrementalRefreshVolume37>(b);
            base_.event_time_ns = m.transactTime();
            auto &entries = m.noMDEntries();
            while (entries.hasNext()) {
                auto &x = entries.next();
                uint64_t id = static_cast<uint32_t>(x.securityID());
                Book &bk = book(id);
                if (!selected(id, bk))
                    continue;
                fq_event e = base_;
                e.type = FQ_STATISTIC;
                e.instrument_id = id;
                e.report_sequence = x.rptSeq();
                e.flags = flags(bk);
                e.data = {};
                e.data.statistic.kind = 'e';
                e.data.statistic.quantity = x.mDEntrySize();
                e.data.statistic.quantity_valid = 1;
                emit_(e);
            }
            if (m.matchEventIndicator().endOfEvent())
                finish();
            return;
        }
        if (type == 49) {
            statistic_group<fq_sbe::MDIncrementalRefreshDailyStatistics49>(b, 0);
            return;
        }
        if (type == 51) {
            statistic_group<fq_sbe::MDIncrementalRefreshSessionStatistics51>(b, 0);
            return;
        }
        if (type == 50) {
            auto m = wrap<fq_sbe::MDIncrementalRefreshLimitsBanding50>(b);
            base_.event_time_ns = m.transactTime();
            auto &entries = m.noMDEntries();
            while (entries.hasNext()) {
                auto &x = entries.next();
                uint64_t id = static_cast<uint32_t>(x.securityID());
                Book &bk = book(id);
                if (!selected(id, bk))
                    continue;
                int64_t p[3] = {x.highLimitPrice().mantissa(), x.lowLimitPrice().mantissa(),
                                x.maxPriceVariation().mantissa()};
                for (unsigned i = 0; i < 3; ++i) {
                    fq_event e = base_;
                    e.type = FQ_STATISTIC;
                    e.instrument_id = id;
                    e.flags = flags(bk);
                    e.data = {};
                    e.data.statistic.kind = 1000 + i;
                    e.data.statistic.price = decimal(p[i], -9, p[i] != INT64_MAX);
                    emit_(e);
                }
            }
            if (m.matchEventIndicator().endOfEvent())
                finish();
            return;
        }
        if (type == 30) {
            auto m = wrap<fq_sbe::SecurityStatus30>(b);
            fq_event e = base_;
            e.type = FQ_TRADING_STATUS;
            e.instrument_id = static_cast<uint32_t>(m.securityID());
            e.event_time_ns = m.transactTime();
            e.data = {};
            e.data.status.code = m.securityTradingStatusRaw();
            if (selected(e.instrument_id, book(e.instrument_id)))
                emit_(e);
            if (m.matchEventIndicator().endOfEvent())
                finish();
            return;
        }
        if (type == 47) {
            auto m = wrap<fq_sbe::MDIncrementalRefreshOrderBook47>(b);
            base_.event_time_ns = m.transactTime();
            auto &entries = m.noMDEntries();
            while (entries.hasNext()) {
                auto &x = entries.next();
                uint64_t id = static_cast<uint32_t>(x.securityID());
                Book &bk = book(id);
                if (!selected(id, bk))
                    continue;
                fq_event e = base_;
                e.type = FQ_ORDER;
                e.instrument_id = id;
                e.flags = flags(bk) & ~FQ_BOOK_VALID;
                e.data = {};
                e.data.order.order_id = x.orderID();
                e.data.order.priority = x.mDOrderPriority();
                e.data.order.priority_valid = x.mDOrderPriority() != UINT64_MAX;
                e.data.order.price =
                    decimal(x.mDEntryPx().mantissa(), -9, x.mDEntryPx().mantissa() != INT64_MAX);
                e.data.order.quantity = x.mDDisplayQty();
                e.data.order.quantity_valid = x.mDDisplayQty() != INT32_MAX;
                e.data.order.side = static_cast<uint8_t>(side(x.mDEntryTypeRaw()));
                e.data.order.action = x.mDUpdateActionRaw();
                emit_(e);
            }
            if (m.matchEventIndicator().endOfEvent())
                finish();
            return;
        }
        if (type == 39) {
            if (b.u8(10 + 31) & 128)
                finish();
            return;
        } // RFQ is not a book update.
        throw ParseError("CME template has no business adapter; raw message is available when enabled");
    }
    void decode_packet(Bytes packet, uint32_t role) {
        base_.send_time_ns = packet.u64(4);
        base_.sequence = packet.u32(0);
        size_t pos = 12;
        uint32_t index = 0;
        while (pos < packet.n) {
            size_t size = packet.u16(pos);
            Bytes b = packet.sub(pos, size);
            base_.message_index = index++;
            const uint16_t type = b.u16(4);
            if (role == FQ_INCREMENTAL && (type == 37 || (type >= 46 && type <= 51)))
                event_open_ = !(b.u8(18) & 128);
            if (role == FQ_INCREMENTAL && type == 30)
                event_open_ =
                    !(b.u8(10 + fq_sbe::SecurityStatus30::matchEventIndicatorEncodingOffset()) & 128);
            if (role == FQ_INCREMENTAL && type == 39)
                event_open_ = !(b.u8(10 + 31) & 128);
            message(b, role);
            pos += size;
        }
    }
    void accept(Bytes b, uint64_t receive, uint32_t flags_) {
        base_.receive_time_ns = receive;
        base_.flags = flags_ & FQ_REPLAYED;
        uint32_t seq = b.u32(0);
        history_.push(seq, b, receive);
        next_ = uint64_t(seq) + 1;
        decode_packet(b, FQ_INCREMENTAL);
    }
    void drain() {
        while (next_ <= UINT32_MAX) {
            auto it = pending_.find(static_cast<uint32_t>(next_));
            if (it == pending_.end())
                break;
            Pending p = std::move(it->second);
            pending_.erase(it);
            accept(Bytes(p.bytes.data(), p.bytes.size()), p.time, p.flags);
        }
        if (pending_.empty())
            gap_started_ = 0;
    }

  public:
    Cme(const FeedConfig &c, Emit e) : Adapter(c, e), history_(c.cache_bytes) {
        books_.reserve(c.max_instruments);
        dirty_.reserve(c.max_instruments);
    }
    uint64_t capabilities() const override {
        return FQ_CAP_RAW | FQ_CAP_MBP | FQ_CAP_TRADES | FQ_CAP_SNAPSHOT | FQ_CAP_ARBITRATION |
               FQ_CAP_ORDER_EVENTS;
    }
    void invalidate(const char *reason) override {
        for (auto &p : books_) {
            p.second.valid = false;
            p.second.pending = false;
            p.second.dirty = false;
            p.second.watermark = 0;
        }
        dirty_.clear();
        history_.clear();
        contiguous_ = false;
        event_open_ = false;
        status(FQ_EPARSE, reason);
    }
    void tick(uint64_t time) override {
        if (!pending_.empty() && time >= gap_started_ && time - gap_started_ >= 2000000) {
            invalidate("CME gap timed out; waiting for fresh snapshots");
            next_ = pending_.begin()->first;
            drain();
            if (!pending_.empty())
                gap_started_ = time;
        }
    }
    void feed(const fq_datagram &d) override {
        prepare(d, FQ_CME);
        Bytes b(d.data, d.size);
        b.require(0, 22);
        if (b.n > History::slot_size)
            throw ParseError("CME datagram too large");
        uint32_t seq = b.u32(0);
        base_.sequence = seq;
        base_.send_time_ns = b.u64(4);
        bool reset = false;
        for (size_t pos = 12; pos < b.n;) {
            size_t n = b.u16(pos);
            if (n < 10)
                throw ParseError("invalid CME MsgSize");
            Bytes m = b.sub(pos, n);
            if (!validate_cme_message(m))
                throw ParseError("unknown CME template");
            if (m.u16(4) == 4) {
                size_t gp = 10 + m.u16(2);
                size_t block = m.u16(gp), count = m.u8(gp + 2);
                for (size_t i = 0; i < count; ++i)
                    if (m.u16(gp + 3 + i * block) == config_.channel)
                        reset = true;
            }
            pos += n;
        }
        if (reset && d.role == FQ_INCREMENTAL) {
            if (base_.send_time_ns <= last_reset_time_)
                return;
            last_reset_time_ = base_.send_time_ns;
            ++epoch_;
            base_.epoch = epoch_;
            invalidate("CME channel reset");
            pending_.clear();
            // Channel reset retains reference data. The recovery incrementals
            // reconstruct an empty book; daily definitions are not necessarily resent.
            for (auto &item : books_) {
                item.second.clear();
                item.second.watermark = 0;
                item.second.pending = true;
            }
            next_ = seq;
            other_next_ = {};
            other_time_ = {};
            contiguous_ = true;
        }
        if (last_reset_time_ && base_.send_time_ns < last_reset_time_)
            return;
        if (d.role != FQ_INCREMENTAL) {
            if (d.role != FQ_SNAPSHOT && d.role != FQ_DEFINITION)
                throw ParseError("invalid CME feed role");
            // Recovery feeds may restart their packet sequence every replay cycle.
            auto &expected = other_next_[d.role];
            auto &time = other_time_[d.role];
            if (base_.send_time_ns < time || (base_.send_time_ns == time && seq < expected))
                return;
            time = base_.send_time_ns;
            expected = uint64_t(seq) + 1;
            decode_packet(b, d.role);
            return;
        }
        tick(d.receive_time_ns);
        if (!next_)
            next_ = seq;
        if (seq < next_)
            return;
        if (seq > next_) {
            if (pending_.count(seq))
                return;
            if (pending_.size() >= 128 || uint64_t(seq) - next_ > 128) {
                status(FQ_ECAPACITY, "CME reorder window exceeded", next_, seq);
                invalidate("CME gap requires snapshot recovery");
                pending_.clear();
                gap_started_ = 0;
                next_ = seq;
            } else {
                if (pending_.empty()) {
                    status(FQ_OK, "CME gap: awaiting redundant line", next_, seq);
                    gap_started_ = d.receive_time_ns;
                }
                Pending p;
                p.bytes.assign(b.p, b.p + b.n);
                p.time = d.receive_time_ns;
                p.flags = d.flags;
                pending_.emplace(seq, std::move(p));
                return;
            }
        }
        accept(b, d.receive_time_ns, d.flags);
        drain();
    }
};
} // namespace
std::unique_ptr<Adapter> make_cme(const FeedConfig &c, Emit e) {
    return std::unique_ptr<Adapter>(new Cme(c, e));
}
} // namespace fqimpl
