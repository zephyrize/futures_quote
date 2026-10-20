#ifndef FQ_TEST_WIRE_FIXTURE_HPP
#define FQ_TEST_WIRE_FIXTURE_HPP
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
using V = std::vector<uint8_t>;
static void put(V &b, size_t p, uint64_t v, size_t n) {
    if (b.size() < p + n)
        b.resize(p + n);
    for (size_t i = 0; i < n; ++i)
        b[p + i] = uint8_t(v >> (8 * i));
}
static void text(V &b, size_t p, const char *s, size_t n) {
    if (p + n > b.size())
        throw std::runtime_error("fixture bounds");
    std::memcpy(&b[p], s, n);
}
static void append(V &a, const V &b) {
    a.insert(a.end(), b.begin(), b.end());
}
static V msg(uint16_t id, uint16_t block, V body) {
    V b(10);
    put(b, 0, body.size() + 10, 2);
    put(b, 2, block, 2);
    put(b, 4, id, 2);
    put(b, 6, 1, 2);
    put(b, 8, 13, 2);
    append(b, body);
    return b;
}
static V packet(uint32_t seq, std::initializer_list<V> messages, uint64_t send = 1000000) {
    V b(12);
    put(b, 0, seq, 4);
    put(b, 4, send, 8);
    for (const auto &m : messages)
        append(b, m);
    return b;
}
static V definition() {
    V b(224);
    b[5] = 'A';
    text(b, 23, "PL", 2);
    text(b, 35, "PLZ6", 4);
    put(b, 55, 42, 4);
    text(b, 76, "USD", 3);
    put(b, 91, 100000000, 8);
    put(b, 99, 1000000000, 8);
    append(b, V{9, 0, 0});
    append(b, V{4, 0, 1, 'G', 'B', 'X', 10});
    append(b, V{4, 0, 0});
    append(b, V{5, 0, 0});
    return msg(54, 224, b);
}
static V quote(uint32_t rpt, int32_t qty, bool end = true, uint8_t action = 1, uint8_t position = 1) {
    V b(11);
    put(b, 0, 123456789, 8);
    b[8] = end ? 128 : 0;
    append(b, V{32, 0, 1});
    size_t p = b.size();
    b.resize(p + 32);
    put(b, p, 100000000000ull, 8);
    put(b, p + 8, uint32_t(qty), 4);
    put(b, p + 12, 42, 4);
    put(b, p + 16, rpt, 4);
    put(b, p + 20, 1, 4);
    b[p + 24] = position;
    b[p + 25] = action;
    b[p + 26] = '0';
    put(b, p + 27, INT32_MAX, 4);
    append(b, V{24, 0, 0, 0, 0, 0, 0, 0});
    return msg(46, 11, b);
}
static V trade() {
    V b(11);
    put(b, 0, 2000, 8);
    b[8] = 128;
    append(b, V{32, 0, 1});
    size_t p = b.size();
    b.resize(p + 32);
    put(b, p, uint64_t(int64_t(-1000000000)), 8);
    put(b, p + 8, 7, 4);
    put(b, p + 12, 42, 4);
    put(b, p + 16, 12, 4);
    put(b, p + 20, 2, 4);
    b[p + 24] = 1;
    put(b, p + 26, 77, 4);
    append(b, V{16, 0, 0, 0, 0, 0, 0, 0});
    return msg(48, 11, b);
}
static V snapshot(uint32_t watermark, uint32_t rpt = 10, int32_t qty = 5) {
    V b(59);
    put(b, 0, watermark, 4);
    put(b, 4, 1, 4);
    put(b, 8, 42, 4);
    put(b, 12, rpt, 4);
    put(b, 16, 1000, 8);
    append(b, V{22, 0, 2});
    for (unsigned s = 0; s < 2; ++s) {
        size_t p = b.size();
        b.resize(p + 22);
        put(b, p, (100 + s) * 1000000000ull, 8);
        put(b, p + 8, uint32_t(qty), 4);
        put(b, p + 12, 1, 4);
        b[p + 16] = 1;
        b[p + 21] = s ? '1' : '0';
    }
    return msg(52, 59, b);
}
static V control(uint16_t type, uint32_t value) {
    V b(8);
    put(b, 0, 8, 2);
    put(b, 2, type, 2);
    put(b, 4, value, 4);
    return b;
}
static V auction(uint64_t price = 123456789) {
    // Independent fixture from LME v4.13 section 5.4.5: two groups, three currencies.
    V b(27);
    put(b, 2, 405, 2);
    put(b, 4, 123000, 8);
    b[12] = 'B';
    put(b, 13, 20261015, 4);
    text(b, 17, "PT", 2);
    text(b, 19, "AM", 2);
    put(b, 21, 10, 2);
    put(b, 23, 11, 2);
    put(b, 25, 2, 2);
    append(b, V{'D', 2, 0});
    size_t p = b.size();
    b.resize(p + 22);
    text(b, p, "USD", 3);
    put(b, p + 3, price, 8);
    text(b, p + 11, "EUR", 3);
    put(b, p + 14, price + 1, 8);
    append(b, V{'M', 1, 0});
    p = b.size();
    b.resize(p + 11);
    text(b, p, "GBP", 3);
    put(b, p + 3, price + 2, 8);
    put(b, 0, b.size(), 2);
    return b;
}
static V lme(uint32_t seq, std::initializer_list<V> messages, uint64_t time = 10000) {
    V b(16);
    b[2] = uint8_t(messages.size());
    put(b, 4, seq, 4);
    put(b, 8, time, 8);
    for (const auto &m : messages)
        append(b, m);
    put(b, 0, b.size(), 2);
    return b;
}
// 以下仅构造 LMEsource v4.13 测试报文，不作为对外编码器；字段偏移来自官方 322/341 定义。
static V lme_book(uint64_t id, uint8_t side, uint8_t position, uint8_t action,
                  int64_t price, uint64_t explicit_qty, uint64_t implied_qty) {
    V b(66);
    put(b, 0, b.size(), 2);
    put(b, 2, 322, 2);
    text(b, 4, "EL", 2);
    put(b, 6, 123000, 8);
    put(b, 14, id, 8);
    b[22] = 1;
    put(b, 23, explicit_qty + implied_qty, 8);
    put(b, 31, static_cast<uint64_t>(price), 8);
    put(b, 39, explicit_qty ? 2 : 0, 4);
    put(b, 43, explicit_qty, 8);
    put(b, 51, implied_qty ? 3 : 0, 4);
    put(b, 55, implied_qty, 8);
    b[63] = side;
    b[64] = position;
    b[65] = action;
    return b;
}
static V lme_trade(uint64_t id, int64_t price, uint32_t qty, uint64_t match, bool cancel = false) {
    V b(63);
    put(b, 0, b.size(), 2);
    put(b, 2, 341, 2);
    put(b, 4, 123000, 8);
    put(b, 12, id, 8);
    text(b, 20, "EL", 2);
    put(b, 22, 123000, 8);
    text(b, 30, "  ", 2);
    b[32] = cancel ? 1 : 0;
    put(b, 33, static_cast<uint64_t>(price), 8);
    put(b, 41, qty, 4);
    put(b, 45, match, 8);
    b[53] = ' ';
    return b;
}
static V lme_clear(uint64_t id) {
    V b(22);
    put(b, 0, b.size(), 2);
    put(b, 2, 327, 2);
    text(b, 4, "EL", 2);
    put(b, 6, 123000, 8);
    put(b, 14, id, 8);
    return b;
}
static V lme_indicative(uint64_t id, int64_t price, int64_t mid, uint32_t volume) {
    V b(44);
    put(b, 0, b.size(), 2);
    put(b, 2, 320, 2);
    text(b, 4, "EL", 2);
    put(b, 6, 123000, 8);
    put(b, 14, id, 8);
    put(b, 22, static_cast<uint64_t>(price), 8);
    put(b, 30, volume, 4);
    put(b, 34, static_cast<uint64_t>(mid), 8);
    return b;
}
static V lme_quote_request(uint64_t id, uint8_t kind, uint8_t side, uint32_t volume) {
    V b(30);
    put(b, 0, b.size(), 2);
    put(b, 2, 329, 2);
    text(b, 4, "EL", 2);
    put(b, 6, 123000, 8);
    put(b, 14, id, 8);
    b[22] = kind;
    b[23] = side;
    put(b, 24, volume, 4);
    return b;
}
static V lme_instrument_state(uint64_t id, uint8_t state, uint8_t condition) {
    V b(44);
    put(b, 0, b.size(), 2);
    put(b, 2, 312, 2);
    text(b, 4, "EL", 2);
    put(b, 6, 123000, 8);
    put(b, 14, id, 8);
    b[22] = 'A';
    b[23] = state;
    b[40] = condition;
    return b;
}
#endif
