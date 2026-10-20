#include <futures_quote/fq.h>
#include <futures_quote/adapter.h>
#include "bytes.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <array>
#include <cstring>
#include <stdexcept>
#include <random>
#include <unistd.h>
#include "wire_fixture.hpp"
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + " " #x);       \
    } while (0)
struct Event {
    fq_event e;
    std::array<std::vector<fq_level>, 4> levels;
};
struct Fixture {
    fq_client *c = 0;
    std::vector<Event> events;
    static void callback(void *p, const fq_event *e) {
        Fixture &f = *static_cast<Fixture *>(p);
        Event v;
        v.e = *e;
        if (e->type == FQ_BOOK)
            for (unsigned s = 0; s < 4; ++s)
                if (e->data.book.counts[s])
                    v.levels[s].assign(e->data.book.sides[s], e->data.book.sides[s] + e->data.book.counts[s]);
        f.events.push_back(v);
    }
    explicit Fixture(const char *path = FQ_SOURCE_DIR "/config/replay.yaml") {
        fq_callbacks cb = {sizeof(cb), FQ_ABI_VERSION, callback, this};
        CHECK(fq_create(&cb, &c) == 0);
        CHECK(fq_open(c, path) == 0);
    }
    ~Fixture() { fq_destroy(c); }
    int feed(V b, uint32_t id = 1, uint32_t role = FQ_INCREMENTAL, uint64_t time = 1000000,
             uint32_t line = 0) {
        fq_datagram d = {sizeof(d), FQ_ABI_VERSION, id, role, b.data(), b.size(), time, FQ_REPLAYED, line};
        return fq_feed(c, &d);
    }
    void ok(V b, uint32_t id = 1, uint32_t role = FQ_INCREMENTAL, uint64_t time = 1000000,
            uint32_t line = 0) {
        int r = feed(b, id, role, time, line);
        if (r) {
            char error[512];
            fq_last_error(c, error, sizeof error);
            throw std::runtime_error(error);
        }
    }
    size_t count(unsigned type) const {
        size_t n = 0;
        for (const auto &e : events)
            if (e.e.type == type)
                ++n;
        return n;
    }
    const Event &last(unsigned type) const {
        for (auto i = events.rbegin(); i != events.rend(); ++i)
            if (i->e.type == type)
                return *i;
        throw std::runtime_error("missing event");
    }
    void ready() {
        ok(packet(1, {definition()}), 1, FQ_DEFINITION);
        ok(packet(100, {quote(11, 3)}));
        ok(packet(1, {snapshot(99)}), 1, FQ_SNAPSHOT);
    }
};
static void cme_recovery() {
    Fixture f;
    f.ready();
    CHECK(f.count(FQ_BOOK) == 1);
    CHECK(f.last(FQ_BOOK).e.flags & FQ_BOOK_VALID);
    CHECK(f.last(FQ_BOOK).levels[0][0].quantity == 3);
    CHECK(f.last(FQ_INSTRUMENT).e.data.instrument.display_factor.mantissa == 1000000000);
    f.ok(packet(101, {quote(12, 4, false)}));
    CHECK(f.count(FQ_BOOK) == 1);
    f.ok(packet(102, {quote(13, 6, true)}));
    CHECK(f.count(FQ_BOOK) == 2);
    CHECK(f.last(FQ_BOOK).levels[0][0].quantity == 6);
    f.ok(packet(104, {quote(15, 8)}), 1, 0, 1100000, 1);
    CHECK(f.count(FQ_GAP) == 1);
    CHECK(f.count(FQ_BOOK) == 2);
    f.ok(packet(103, {quote(14, 7)}), 1, 0, 1200000);
    CHECK(f.count(FQ_BOOK) == 4);
    CHECK(f.last(FQ_BOOK).levels[0][0].quantity == 8);
    f.ok(packet(104, {quote(15, 8)}));
    CHECK(f.count(FQ_BOOK) == 4);
    f.ok(packet(105, {trade()}));
    CHECK(f.last(FQ_TRADE).e.data.trade.price.mantissa == -1000000000);
    CHECK(f.last(FQ_TRADE).e.data.trade.quantity == 7);
    f.ok(packet(107, {quote(17, 10)}), 1, 0, 2000000);
    f.ok(packet(108, {quote(18, 11)}), 1, 0, 6000000);
    CHECK(f.count(FQ_BOOK) == 4);
    CHECK(!(f.last(FQ_BOOK_DELTA).e.flags & FQ_BOOK_VALID));
    f.ok(packet(2, {snapshot(106, 16, 9)}), 1, FQ_SNAPSHOT, 7000000);
    CHECK(f.count(FQ_BOOK) == 5);
    CHECK(f.last(FQ_BOOK).levels[0][0].quantity == 11);
    CHECK(!(f.last(FQ_BOOK).e.flags & FQ_STREAM_CONTIGUOUS));
}
static void cme_validation() {
    Fixture f;
    V p = packet(1, {quote(1, 1)});
    for (size_t n = 1; n < p.size(); ++n) {
        V truncated(p.begin(), p.begin() + n);
        CHECK(f.feed(truncated) < 0);
    }
    V bad = p;
    put(bad, 12 + 6, 2, 2);
    CHECK(f.feed(bad) < 0);
    bad = p;
    put(bad, 12 + 8, 14, 2);
    CHECK(f.feed(bad) < 0);
    bad = p;
    put(bad, 12 + 10 + 11, 1, 2);
    CHECK(f.feed(bad) < 0); // group block too short, despite enough packet bytes
    bad = p;
    bad[12 + 10 + 11 + 2] = 255;
    CHECK(f.feed(bad) < 0);
    bad = p;
    put(bad, 12 + 4, 999, 2);
    CHECK(f.feed(bad) < 0);
    CHECK(f.count(FQ_BOOK) == 0);
}
static void cme_same_packet_and_empty_snapshot() {
    Fixture f;
    f.ok(packet(1, {definition()}), 1, FQ_DEFINITION);
    f.ok(packet(100, {quote(11, 3, false), quote(12, 4, true)}));
    f.ok(packet(1, {snapshot(99)}), 1, FQ_SNAPSHOT);
    CHECK(f.last(FQ_BOOK).levels[0][0].quantity == 4);
    f.ok(packet(101, {quote(13, 0, true, 2)}));
    CHECK(f.last(FQ_BOOK).levels[0].empty());
    f.ok(packet(102, {quote(14, 7, true, 0)}));
    CHECK(f.last(FQ_BOOK).levels[0][0].quantity == 7);
    V bad = packet(103, {quote(15, 1, true, 1, 4)});
    CHECK(f.feed(bad) < 0);
}
static void lme_arbitration_refresh() {
    Fixture f;
    f.ok(lme(100, {auction(100), auction(200), auction(300)}), 2);
    CHECK(f.count(FQ_REFERENCE_PRICE) == 0);
    f.ok(lme(1, {control(203, 99)}), 2, FQ_SNAPSHOT);
    f.ok(lme(2, {auction(50), control(203, 99)}), 2, FQ_SNAPSHOT);
    CHECK(f.count(FQ_REFERENCE_PRICE) == 12);
    CHECK(f.last(FQ_REFERENCE_PRICE).e.data.reference_price.price.mantissa == 302);
    f.ok(lme(102, {auction(300), auction(400), auction(500)}), 2, 0, 1100000, 1);
    CHECK(f.count(FQ_REFERENCE_PRICE) == 18);
    CHECK(f.last(FQ_REFERENCE_PRICE).e.sequence == 104);
    f.ok(lme(104, {}), 2);
    CHECK(f.count(FQ_REFERENCE_PRICE) == 18);
    f.ok(lme(106, {auction(700)}), 2, 0, 1200000);
    CHECK(f.count(FQ_REFERENCE_PRICE) == 18);
    f.ok(lme(105, {auction(600)}), 2, 0, 1300000, 1);
    CHECK(f.count(FQ_REFERENCE_PRICE) == 24);
    CHECK(std::strcmp(f.last(FQ_REFERENCE_PRICE).e.data.reference_price.currency, "GBP") == 0);
    f.ok(lme(108, {auction(900)}), 2, 0, 2000000);
    f.ok(lme(109, {auction(1000)}), 2, 0, 6000000);
    CHECK(f.count(FQ_REFERENCE_PRICE) == 24);
    f.ok(lme(4, {control(203, 107)}), 2, FQ_SNAPSHOT);
    f.ok(lme(5, {auction(800), control(203, 107)}), 2, FQ_SNAPSHOT);
    CHECK(f.count(FQ_REFERENCE_PRICE) == 33);
    CHECK(f.last(FQ_REFERENCE_PRICE).e.sequence == 109);
}
static void lme_validation() {
    Fixture f;
    V p = lme(1, {auction()});
    for (size_t n = 1; n < p.size(); ++n)
        CHECK(f.feed(V(p.begin(), p.begin() + n), 2) < 0);
    V bad = p;
    bad[2] = 2;
    CHECK(f.feed(bad, 2) < 0);
    bad = p;
    put(bad, 16 + 25, 65535, 2);
    CHECK(f.feed(bad, 2) < 0);
    CHECK(f.feed(lme(1, {auction(uint64_t(INT64_MAX) + 1)}), 2) < 0);
    // A truncated second message must not allow first-message publication.
    f.ok(lme(1, {control(203, 0)}), 2, FQ_SNAPSHOT);
    f.ok(lme(2, {auction(), control(203, 0)}), 2, FQ_SNAPSHOT);
    CHECK(f.count(FQ_REFERENCE_PRICE) == 0);
}

static void cme_controls_and_metadata() {
    Fixture f;
    f.ready();
    // A non-book message may close an event spanning packets.
    f.ok(packet(101, {quote(12, 4, false)}));
    V status(30);
    put(status, 16, 42, 4);
    status[26] = 128;
    f.ok(packet(102, {msg(30, 30, status)}));
    CHECK(f.count(FQ_BOOK) == 2);
    CHECK(f.last(FQ_BOOK).levels[0][0].quantity == 4);
    V reset(9);
    reset[8] = 128;
    append(reset, V{2, 0, 1, 128, 1});
    f.ok(packet(1, {msg(4, 9, reset)}, 2000000));
    size_t count = f.count(FQ_BOOK);
    f.ok(packet(103, {quote(13, 6)}, 1000000)); // previous epoch arriving late
    CHECK(f.count(FQ_BOOK) == count);
    CHECK(f.last(FQ_STATUS).e.epoch == 2);
    V rebuilt = quote(1, 9, true, 0);
    rebuilt[18] |= 32;
    f.ok(packet(2, {rebuilt}, 2000001));
    CHECK(f.count(FQ_BOOK) == count + 1);
    CHECK(f.last(FQ_BOOK).e.flags & FQ_BOOK_VALID);
    CHECK(f.last(FQ_BOOK).levels[0][0].quantity == 9);
    for (unsigned type = 55; type <= 56; ++type) {
        unsigned block = type == 55 ? 221 : 255;
        V def(block);
        def[5] = 'A';
        text(def, 23, "PL", 2);
        text(def, 35, "PL option", 9);
        put(def, 55, 40 + type, 4);
        text(def, type == 55 ? 77 : 76, "USD", 3);
        put(def, type == 55 ? 111 : 94, INT64_MAX, 8);
        put(def, type == 55 ? 127 : 102, 1000000000, 8);
        append(def, V{9, 0, 0, 4, 0, 1, 'G', 'B', 'X', 10, 4, 0, 0, 5, 0, 0});
        if (type == 55)
            append(def, V{24, 0, 0, 24, 0, 0});
        else
            append(def, V{18, 0, 0});
        f.ok(packet(type, {msg(type, block, def)}, 2000001), 1, FQ_DEFINITION);
        CHECK(f.last(FQ_INSTRUMENT).e.instrument_id == 40 + type);
        CHECK(!f.last(FQ_INSTRUMENT).e.data.instrument.tick_size.valid);
    }
    Fixture atomic;
    V malformed = packet(1, {trade(), quote(1, 1)});
    malformed.pop_back();
    CHECK(atomic.feed(malformed) < 0);
    CHECK(atomic.count(FQ_TRADE) == 0);
}
static void lme_controls_and_idle() {
    Fixture f;
    f.ok(lme(100, {auction(100)}, 12345), 2);
    f.ok(lme(1, {control(203, 99)}), 2, FQ_SNAPSHOT);
    f.ok(lme(2, {auction(50), control(203, 99)}), 2, FQ_SNAPSHOT);
    CHECK(f.last(FQ_REFERENCE_PRICE).e.send_time_ns == 12345);
    size_t n = f.count(FQ_REFERENCE_PRICE);
    f.ok(lme(900, {control(100, 1)}, 20000), 2, FQ_SNAPSHOT);
    f.ok(lme(101, {auction(200)}, 20001), 2);
    CHECK(f.count(FQ_REFERENCE_PRICE) == n + 3); // refresh reset leaves realtime intact
    f.ok(lme(1, {control(105, 1)}, 30000), 2, FQ_DR);
    f.ok(lme(102, {auction(300)}, 30001), 2);
    CHECK(f.count(FQ_REFERENCE_PRICE) == n + 3);
    f.ok(lme(2, {control(105, 2)}, 40000), 2, FQ_DR);
    f.ok(lme(200, {auction(400)}, 40001), 2);
    f.ok(lme(1, {control(203, 199)}, 40001), 2, FQ_SNAPSHOT);
    f.ok(lme(2, {auction(399), control(203, 199)}, 40001), 2, FQ_SNAPSHOT);
    n = f.count(FQ_REFERENCE_PRICE);
    f.ok(lme(3, {control(105, 2)}, 50000), 2, FQ_DR);
    f.ok(lme(201, {auction(500)}, 50001), 2);
    CHECK(f.count(FQ_REFERENCE_PRICE) == n + 3); // repeated DR-completed is not another reset
    Fixture idle;
    idle.ok(lme(100, {}), 2);
    idle.ok(lme(1, {control(203, 100)}), 2, FQ_SNAPSHOT);
    idle.ok(lme(2, {auction(50), control(203, 100)}), 2, FQ_SNAPSHOT);
    CHECK(idle.count(FQ_REFERENCE_PRICE) == 3);
    idle.ok(lme(101, {auction(60)}), 2);
    CHECK(idle.count(FQ_REFERENCE_PRICE) == 6);
}
static void bounded_recovery() {
    std::string path = "/tmp/fq-small-" + std::to_string(getpid()) + ".yaml";
    {
        std::ofstream out(path.c_str());
        out << "config_version: 1\nfeeds:\n  - {id: 1, protocol: cme_mdp3, revision: '13', channel_id: 384, "
               "recovery_buffer_bytes: 32768}\n";
    }
    {
        Fixture f(path.c_str());
        f.ok(packet(1, {definition()}), 1, FQ_DEFINITION);
        f.ok(packet(100, {quote(11, 3)}));
        f.ok(packet(101, {quote(12, 4)}));
        f.ok(packet(102, {quote(13, 5)}));
        f.ok(packet(1, {snapshot(99)}), 1, FQ_SNAPSHOT);
        CHECK(f.count(FQ_BOOK) == 0);
        f.ok(packet(2, {snapshot(101, 12)}), 1, FQ_SNAPSHOT);
        CHECK(f.count(FQ_BOOK) == 1);
        CHECK(f.last(FQ_BOOK).levels[0][0].quantity == 5);
    }
    std::remove(path.c_str());
}
// 删除私有协议后，旧配置必须明确失败，不能自动回退到 CME 原生解码器。
static void removed_protocol_rejected() {
    std::string path = "/tmp/fq-removed-protocol-" + std::to_string(getpid()) + ".yaml";
    {
        std::ofstream out(path.c_str());
        out << "config_version: 1\nfeeds:\n  - {id: 1, protocol: vendor_cme_v1, revision: '1', channel_id: "
               "1}\n";
    }
    char error[512] = {};
    CHECK(fq_check_config(path.c_str(), error, sizeof error) == FQ_EINVAL);
    CHECK(std::strstr(error, "unregistered protocol: vendor_cme_v1") != nullptr);
    Fixture f;
    CHECK(fq_open(f.c, path.c_str()) == FQ_EINVAL);
    CHECK(fq_last_error(f.c, error, sizeof error) > 1);
    CHECK(std::strstr(error, "unregistered protocol: vendor_cme_v1") != nullptr);
    std::remove(path.c_str());
}
static void config_and_lifecycle() {
    Fixture f;
    CHECK((fq_capabilities(f.c, 1) & FQ_CAP_MBP) != 0);
    CHECK(!(fq_capabilities(f.c, 1) & FQ_CAP_MBO));
    CHECK(fq_start(f.c) == FQ_EIO); // unspecified local interface must fail before sockets are opened
    char error[512];
    CHECK(fq_last_error(f.c, error, sizeof error) > 1);
    fq_datagram invalid = {};
    CHECK(fq_feed(f.c, &invalid) == FQ_EINVAL);
    fq_request_stop(f.c);
    CHECK(f.feed(packet(1, {trade()})) == FQ_ESTATE);
    CHECK(fq_join(f.c) == FQ_OK);
    CHECK(fq_open(f.c, FQ_SOURCE_DIR "/config/replay.yaml") == 0);
    std::string path = "/tmp/fq-invalid-" + std::to_string(getpid()) + ".yaml";
    {
        std::ofstream out(path.c_str());
        out << "config_version: 1\nunknown_key: yes\nfeeds: []\n";
    }
    CHECK(fq_check_config(path.c_str(), error, sizeof error) < 0);
    std::remove(path.c_str());
}
struct Custom {
    fq_on_event emit;
    void *context;
    uint32_t feed, channel;
};
static void *custom_create(const fq_adapter_options *o, fq_on_event emit, void *context) {
    if (!o->feed_config_yaml || !std::strstr(o->feed_config_yaml, "test_exchange"))
        return nullptr;
    Custom *c = new Custom;
    c->emit = emit;
    c->context = context;
    c->feed = o->feed_id;
    c->channel = o->channel_id;
    return c;
}
static int custom_feed(void *p, const fq_datagram *d) {
    Custom &c = *static_cast<Custom *>(p);
    fq_event e = {};
    e.struct_size = sizeof e;
    e.abi_version = 1;
    e.type = FQ_TRADE;
    e.exchange = FQ_CUSTOM;
    e.feed_id = c.feed;
    e.channel_id = c.channel;
    e.data.trade.price.mantissa = d->data[0];
    c.emit(c.context, &e);
    return 0;
}
static void custom_destroy(void *p) {
    delete static_cast<Custom *>(p);
}
static void extensibility() {
    fq_adapter_v1 a = {sizeof a,      1,           "test_exchange", FQ_CAP_TRADES,
                       custom_create, custom_feed, custom_destroy};
    CHECK(fq_register_adapter(&a) == 0);
    CHECK(fq_register_adapter(&a) == FQ_ESTATE);
    std::string path = "/tmp/fq-custom-" + std::to_string(getpid()) + ".yaml";
    {
        std::ofstream out(path.c_str());
        out << "config_version: 1\nfeeds:\n  - {id: 3, protocol: test_exchange, revision: '1', channel_id: "
               "7}\n";
    }
    {
        Fixture f(path.c_str());
        f.ok(V{42}, 3);
        CHECK(f.last(FQ_TRADE).e.data.trade.price.mantissa == 42);
        CHECK(fq_capabilities(f.c, 3) == FQ_CAP_TRADES);
    }
    std::remove(path.c_str());
}
static void throwing_callback(void *, const fq_event *) {
    throw std::runtime_error("consumer failure");
}
static void callback_contract() {
    fq_callbacks cb = {sizeof cb, 1, throwing_callback, 0};
    fq_client *c = 0;
    CHECK(fq_create(&cb, &c) == 0);
    CHECK(fq_open(c, FQ_SOURCE_DIR "/config/replay.yaml") == 0);
    V p = packet(1, {trade()});
    fq_datagram d = {sizeof d, 1, 1, 0, p.data(), p.size(), 1000, 0, 0};
    CHECK(fq_feed(c, &d) == FQ_ECALLBACK);
    fq_destroy(c);
}
static void malformed_fuzz() {
    Fixture f;
    std::mt19937 random(12345);
    V original = packet(1, {quote(1, 1)});
    for (unsigned i = 0; i < 1000; ++i) {
        V b = original;
        b[random() % b.size()] = uint8_t(random());
        f.feed(b);
    }
    for (unsigned i = 0; i < 1000; ++i) {
        V b = lme(1, {auction()});
        b[random() % b.size()] = uint8_t(random());
        f.feed(b, 2);
    }
}
static void byte_reading() {
    uint8_t b[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    fqimpl::Bytes v(b, 8);
    CHECK(v.i64(0) == -1);
    CHECK(v.i32(1) == -1);
    CHECK(v.u16(3) == 65535);
    bool failed = false;
    try {
        v.u64(1);
    } catch (...) {
        failed = true;
    }
    CHECK(failed);
}
static void lme_core_l2_and_trade_replay() {
    const std::string path = "/tmp/fq-lme-core-" + std::to_string(getpid()) + ".yaml";
    {
        std::ofstream out(path.c_str());
        out << "config_version: 1\nfeeds:\n"
               "  - {id: 112, protocol: lmesource_v4, revision: '4.13', channel_id: 112, depth: 15}\n"
               "  - {id: 614, protocol: lmesource_v4, revision: '4.13', channel_id: 614}\n";
    }
    {
        Fixture f(path.c_str());
        CHECK(fq_capabilities(f.c, 112) & FQ_CAP_MBP);
        CHECK(fq_capabilities(f.c, 614) & FQ_CAP_TRADES);
        // 112 增量先入恢复环；612 两个 203 之间的 322 为基线，结束水位为 99。
        f.ok(lme(100, {lme_book(42, 'B', 1, 1, 100000000, 30, 7)}), 112);
        CHECK(f.count(FQ_BOOK) == 0);
        f.ok(lme(1, {control(203, 98)}), 112, FQ_SNAPSHOT);
        f.ok(lme(2, {lme_instrument_state(42, 2, ' '),
                     lme_book(42, 'B', 1, 0, 100000000, 20, 5),
                     lme_indicative(42, 100500000, INT64_MIN, 11), control(203, 99)}),
             112, FQ_SNAPSHOT);
        CHECK(f.count(FQ_BOOK) == 1);
        CHECK(f.last(FQ_BOOK).e.flags & FQ_BOOK_VALID);
        CHECK(f.last(FQ_BOOK).levels[FQ_BID][0].quantity == 30);
        CHECK(f.last(FQ_BOOK).levels[FQ_IMPLIED_BID][0].quantity == 7);
        CHECK(f.last(FQ_BOOK).levels[FQ_IMPLIED_BID][0].order_count == 3);
        CHECK(f.last(FQ_BOOK).e.sequence == 99); // 612 消息序号不能充当 112 水位。
        CHECK(f.count(FQ_INDICATIVE) == 1);
        CHECK(f.last(FQ_INDICATIVE).e.flags & FQ_RECOVERED);
        CHECK(f.last(FQ_INDICATIVE).e.data.indicative.price.mantissa == 100500000);
        CHECK(!f.last(FQ_INDICATIVE).e.data.indicative.mid_price.valid);
        CHECK(f.last(FQ_TRADING_STATUS).e.data.status.code == 2);
        CHECK(f.last(FQ_TRADING_STATUS).e.flags & FQ_RECOVERED);
        f.ok(lme(101, {lme_book(42, 'B', 1, 0, 101000000, 10, 0)}), 112);
        CHECK(f.last(FQ_BOOK).levels[FQ_BID].size() == 2);
        CHECK(f.last(FQ_BOOK).levels[FQ_BID][1].quantity == 30);
        f.ok(lme(102, {lme_book(42, 'B', 2, 2, 100000000, 30, 7)}), 112);
        CHECK(f.last(FQ_BOOK).levels[FQ_BID].size() == 1);
        f.ok(lme(103, {lme_trade(42, 101000000, 4, 999)}), 112);
        CHECK(f.last(FQ_TRADE).e.data.trade.trade_id == 999);
        CHECK(f.last(FQ_TRADE).e.data.trade.quantity == 4);
        f.ok(lme(104, {lme_quote_request(42, 1, 'B', 8)}), 112);
        CHECK(f.last(FQ_QUOTE_REQUEST).e.data.quote_request.volume == 8);
        CHECK(f.last(FQ_QUOTE_REQUEST).e.data.quote_request.side == 'B');
        f.ok(lme(105, {lme_clear(42)}), 112);
        CHECK(f.last(FQ_BOOK).levels[FQ_BID].empty());
        size_t trades = f.count(FQ_TRADE);
        // 614 有自己的 203 周期与序号；A/B 重复消息只发布一次。
        f.ok(lme(1, {control(203, 0)}), 614, FQ_SNAPSHOT);
        f.ok(lme(2, {lme_trade(42, 99000000, 2, 900)}), 614, FQ_SNAPSHOT);
        f.ok(lme(2, {lme_trade(42, 99000000, 2, 900)}), 614, FQ_SNAPSHOT, 1000000, 1);
        f.ok(lme(3, {control(203, 100)}), 614, FQ_SNAPSHOT);
        CHECK(f.count(FQ_TRADE) == trades + 1);
        CHECK(f.last(FQ_TRADE).e.feed_id == 614);
        CHECK(f.last(FQ_TRADE).e.flags & FQ_RECOVERED);
        V bad = lme(106, {lme_book(42, 'B', 1, 0, 100000000, 1, 1)});
        bad[16 + 65] = 9;
        CHECK(f.feed(bad, 112) < 0);
    }
    {
        Fixture f(path.c_str());
        f.ok(lme(100, {lme_book(42, 'B', 1, 0, 100000000, 10, 1)}), 112);
        f.ok(lme(102, {lme_book(42, 'B', 1, 1, 100000000, 20, 2)}), 112);
        f.ok(lme(1, {control(203, 99)}), 112, FQ_SNAPSHOT);
        f.ok(lme(2, {lme_book(42, 'B', 1, 0, 100000000, 15, 1), control(203, 101)}),
             112, FQ_SNAPSHOT);
        CHECK(f.count(FQ_BOOK) == 1);
        CHECK(f.last(FQ_BOOK).e.flags & FQ_BOOK_VALID);
        CHECK(f.last(FQ_BOOK).levels[FQ_BID][0].quantity == 20);
    }
    {
        Fixture f(path.c_str());
        // 第二条消息越过包尾：公共包视图必须在发布第一笔成交前拒绝整个包。
        V bad = lme(10, {lme_trade(42, 100000000, 1, 1),
                         lme_trade(42, 100000000, 1, 2)});
        put(bad, 16 + 63, 1000, 2);
        CHECK(f.feed(bad, 112) < 0);
        CHECK(f.count(FQ_TRADE) == 0);
        f.ok(lme(1, {control(203, 0)}), 614, FQ_SNAPSHOT);
        CHECK(f.feed(bad, 614, FQ_SNAPSHOT) < 0);
        CHECK(f.count(FQ_TRADE) == 0);
    }
    std::remove(path.c_str());
}
int main() {
    struct Test {
        const char *name;
        void (*run)();
    } tests[] = {{"byte_reading", byte_reading},
                 {"cme_recovery", cme_recovery},
                 {"cme_validation", cme_validation},
                 {"cme_event_boundary", cme_same_packet_and_empty_snapshot},
                 {"lme_arbitration_refresh", lme_arbitration_refresh},
                 {"lme_validation", lme_validation},
                 {"lme_core_l2_trade_replay", lme_core_l2_and_trade_replay},
                 {"cme_controls_metadata", cme_controls_and_metadata},
                 {"lme_controls_idle", lme_controls_and_idle},
                 {"bounded_recovery", bounded_recovery},
                 {"removed_protocol", removed_protocol_rejected},
                 {"config_lifecycle", config_and_lifecycle},
                 {"extensibility", extensibility},
                 {"callback_contract", callback_contract},
                 {"malformed_fuzz", malformed_fuzz}};
    unsigned failures = 0;
    for (const auto &t : tests)
        try {
            t.run();
            std::cout << "PASS " << t.name << '\n';
        } catch (const std::exception &e) {
            ++failures;
            std::cerr << "FAIL " << t.name << ": " << e.what() << '\n';
        }
    return failures ? 1 : 0;
}
