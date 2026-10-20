#include <futures_quote/client.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#include <vector>
namespace {
volatile std::sig_atomic_t stopped = 0;
void signal_stop(int) {
    stopped = 1;
}
std::string price(fq_decimal p) {
    if (!p.valid)
        return "null";
    uint64_t v = p.mantissa < 0 ? uint64_t(-(p.mantissa + 1)) + 1 : uint64_t(p.mantissa);
    std::string digits = std::to_string(v);
    if (p.exponent < 0) {
        size_t decimals = static_cast<size_t>(-int(p.exponent));
        if (digits.size() <= decimals)
            digits.insert(0, decimals + 1 - digits.size(), '0');
        digits.insert(digits.size() - decimals, 1, '.');
    } else
        digits.append(static_cast<size_t>(p.exponent), '0');
    return p.mantissa < 0 ? "-" + digits : digits;
}
const char *type_name(uint32_t type) {
    switch (type) {
    case FQ_STATUS: return "STATUS";
    case FQ_GAP: return "GAP";
    case FQ_INSTRUMENT: return "INSTRUMENT";
    case FQ_BOOK: return "BOOK";
    case FQ_BOOK_DELTA: return "BOOK_DELTA";
    case FQ_TRADE: return "TRADE";
    case FQ_STATISTIC: return "STATISTIC";
    case FQ_REFERENCE_PRICE: return "REFERENCE_PRICE";
    case FQ_RAW_MESSAGE: return "RAW_MESSAGE";
    case FQ_ORDER: return "ORDER";
    case FQ_TRADING_STATUS: return "TRADING_STATUS";
    case FQ_INDICATIVE: return "INDICATIVE";
    case FQ_QUOTE_REQUEST: return "QUOTE_REQUEST";
    default: return "UNKNOWN";
    }
}
const char *side_name(unsigned side) {
    static const char *names[] = {"bid", "ask", "implied_bid", "implied_ask"};
    return side < 4 ? names[side] : "unknown";
}
const char *action_name(unsigned action) {
    static const char *names[] = {"new", "change", "delete", "delete_through", "delete_from", "overlay", "clear"};
    return action < 7 ? names[action] : "unknown";
}
void print_level(const fq_level &level) {
    std::cout << price(level.price) << '/';
    if (level.quantity_valid)
        std::cout << level.quantity;
    else
        std::cout << "null";
    std::cout << '/';
    if (level.order_count_valid)
        std::cout << level.order_count;
    else
        std::cout << "null";
}
struct Copy {
    fq_event event = {};
    std::array<std::array<fq_level, FQ_MAX_DEPTH>, 4> levels;
    std::array<uint8_t, 65536> raw;
    void assign(const fq_event &e) {
        event = e;
        if (e.type == FQ_BOOK)
            for (unsigned s = 0; s < 4; ++s) {
                if (e.data.book.counts[s] > FQ_MAX_DEPTH)
                    throw std::runtime_error("book exceeds printer capacity");
                if (e.data.book.counts[s])
                    std::memcpy(levels[s].data(), e.data.book.sides[s],
                                e.data.book.counts[s] * sizeof(fq_level));
                event.data.book.sides[s] = levels[s].data();
            }
        if (e.type == FQ_RAW_MESSAGE) {
            if (e.data.raw.size > raw.size())
                throw std::runtime_error("raw message exceeds printer capacity");
            std::memcpy(raw.data(), e.data.raw.data, e.data.raw.size);
            event.data.raw.data = raw.data();
        }
    }
};
void print(const fq_event &e) {
    std::cout << "feed=" << e.feed_id << " channel=" << e.channel_id << " seq=" << e.sequence
              << " type=" << e.type << " type_name=" << type_name(e.type)
              << " instrument=" << e.instrument_id << " epoch=" << e.epoch
              << " rpt_seq=" << e.report_sequence << " msg_idx=" << e.message_index
              << " flags=" << e.flags << " book_valid=" << !!(e.flags & FQ_BOOK_VALID)
              << " metadata_ready=" << !!(e.flags & FQ_METADATA_READY)
              << " contiguous=" << !!(e.flags & FQ_STREAM_CONTIGUOUS)
              << " recovered=" << !!(e.flags & FQ_RECOVERED)
              << " event_complete=" << !!(e.flags & FQ_EVENT_COMPLETE);
    switch (e.type) {
    case FQ_STATUS:
    case FQ_GAP:
    case FQ_TRADING_STATUS:
        std::cout << " code=" << e.data.status.code << " " << e.data.status.text
                  << " expected=" << e.data.status.expected << " received=" << e.data.status.received;
        break;
    case FQ_INSTRUMENT:
        std::cout << " symbol=" << e.data.instrument.symbol << " group=" << e.data.instrument.security_group
                  << " currency=" << e.data.instrument.currency << " depth=" << e.data.instrument.depth
                  << " action=" << e.data.instrument.action
                  << " tick_size=" << price(e.data.instrument.tick_size)
                  << " display_factor=" << price(e.data.instrument.display_factor);
        break;
    case FQ_TRADE:
        std::cout << " price=" << price(e.data.trade.price) << " qty=";
        if (e.data.trade.quantity_valid)
            std::cout << e.data.trade.quantity;
        else
            std::cout << "null";
        std::cout << " orders=" << e.data.trade.order_count
                  << " aggressor=" << unsigned(e.data.trade.aggressor)
                  << " action=" << unsigned(e.data.trade.action) << " trade_id=";
        if (e.data.trade.trade_id_valid)
            std::cout << e.data.trade.trade_id;
        else
            std::cout << "null";
        break;
    case FQ_BOOK:
        for (unsigned s = 0; s < 4; ++s) {
            std::cout << ' ' << side_name(s) << "=[";
            for (unsigned i = 0; i < e.data.book.counts[s]; ++i) {
                const fq_level &l = e.data.book.sides[s][i];
                if (i)
                    std::cout << ',';
                print_level(l);
            }
            std::cout << ']';
        }
        break;
    case FQ_BOOK_DELTA:
        std::cout << " level=" << e.data.delta.position << " side=" << side_name(e.data.delta.side)
                  << " action=" << action_name(e.data.delta.action) << " price_qty_orders=";
        print_level(e.data.delta.level);
        break;
    case FQ_REFERENCE_PRICE: {
        const auto &p = e.data.reference_price;
        std::cout << " commodity=" << p.commodity << " session=" << p.session << " currency=" << p.currency
                  << " price_type=" << p.price_type << " date=" << p.business_date
                  << " price=" << price(p.price);
        break;
    }
    case FQ_INDICATIVE:
        std::cout << " indicative_price=" << price(e.data.indicative.price)
                  << " indicative_mid=" << price(e.data.indicative.mid_price)
                  << " indicative_volume=" << e.data.indicative.volume;
        break;
    case FQ_QUOTE_REQUEST:
        std::cout << " request_type=" << unsigned(e.data.quote_request.request_type)
                  << " side=" << (e.data.quote_request.side ? char(e.data.quote_request.side) : '-')
                  << " amended_delta_qty=" << e.data.quote_request.volume;
        break;
    case FQ_STATISTIC:
        std::cout << " kind=" << e.data.statistic.kind << " price=" << price(e.data.statistic.price)
                  << " qty=" << e.data.statistic.quantity;
        break;
    case FQ_ORDER:
        std::cout << " order_id=" << e.data.order.order_id << " price=" << price(e.data.order.price)
                  << " qty=" << e.data.order.quantity << " action=" << unsigned(e.data.order.action);
        break;
    case FQ_RAW_MESSAGE:
        std::cout << " message_type=" << e.message_type << " bytes=" << e.data.raw.size;
        break;
    default:
        break;
    }
    std::cout << " event_ns=" << e.event_time_ns << " send_ns=" << e.send_time_ns
              << " recv_ns=" << e.receive_time_ns << '\n';
}
// 打印器与解码线程用单生产者/单消费者队列解耦。限速在深复制前执行，避免终端慢时
// 持续占用队列并反压收包；状态、缺口、交易状态不采样，便于现场诊断。
class Printer {
    std::vector<Copy> slots_;
    std::atomic<size_t> head_{0}, tail_{0};
    std::atomic<bool> done_{false};
    std::thread thread_;
    struct Window {
        std::chrono::steady_clock::time_point begin;
        uint64_t accepted = 0;
        uint64_t ordinary = 0;
    };
    // 每个 feed 独立限速，避免繁忙的 382 将较少更新的 384 完全挤出打印窗口。
    std::map<uint32_t, Window> windows_;

  public:
    fq_client *client = 0;
    std::atomic<uint64_t> seen{0}, sampled{0}, overflow{0};
    uint64_t maximum = 0, rate = 100;
    Printer() : slots_(256), thread_([this] { run(); }) {}
    ~Printer() {
        done_ = true;
        if (thread_.joinable())
            thread_.join();
    }
    static void callback(void *user, const fq_event *e) {
        Printer &p = *static_cast<Printer *>(user);
        uint64_t count = ++p.seen;
        // rate=0 关闭采样；控制类事件始终入队。普通类型最多占用一半配额，
        // 为 BOOK/TRADE 留出打印空间，防止定义或盘口逐档更新淹没核心行情。
        bool control = e->type == FQ_STATUS || e->type == FQ_GAP || e->type == FQ_TRADING_STATUS;
        if (p.rate && !control) {
            auto now = std::chrono::steady_clock::now();
            Window &window = p.windows_[e->feed_id];
            if (now - window.begin >= std::chrono::seconds(1)) {
                window.begin = now;
                window.accepted = 0;
                window.ordinary = 0;
            }
            bool core = e->type == FQ_BOOK || e->type == FQ_TRADE;
            uint64_t ordinary_limit = p.rate / 2 + p.rate % 2;
            if (window.accepted >= p.rate || (!core && window.ordinary >= ordinary_limit)) {
                ++p.sampled;
                if (stopped || (p.maximum && count >= p.maximum))
                    fq_request_stop(p.client);
                return;
            }
            ++window.accepted;
            if (!core)
                ++window.ordinary;
        }
        size_t h = p.head_.load(std::memory_order_relaxed), next = (h + 1) % p.slots_.size();
        if (next == p.tail_.load(std::memory_order_acquire)) {
            ++p.overflow;
        } else {
            p.slots_[h].assign(*e);
            p.head_.store(next, std::memory_order_release);
        }
        if (stopped || (p.maximum && count >= p.maximum))
            fq_request_stop(p.client);
    }
    void run() {
        while (true) {
            size_t t = tail_.load(std::memory_order_relaxed);
            if (t == head_.load(std::memory_order_acquire)) {
                if (done_)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const auto &e = slots_[t].event;
            print(e);
            tail_.store((t + 1) % slots_.size(), std::memory_order_release);
        }
    }
    void finish() {
        done_ = true;
        if (thread_.joinable())
            thread_.join();
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        std::string config, pcap;
        bool check = false;
        uint64_t duration = 0, maximum = 0, rate = 100;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--help") {
                std::cout << "fq_dump --config FILE [--check-config | --pcap FILE] [--duration SECONDS] "
                             "[--max-events N] [--rate N]\n--rate N queues at most N non-control "
                             "events per feed per second for printing (default 100); 0 disables sampling. "
                             "Prices are wire decimals, not display-factor adjusted.\n";
                return 0;
            }
            if (a == "--check-config") {
                check = true;
                continue;
            }
            if (i + 1 >= argc)
                throw std::runtime_error("missing value for " + a);
            std::string value = argv[++i];
            if (a == "--config")
                config = value;
            else if (a == "--pcap")
                pcap = value;
            else if (a == "--duration")
                duration = std::stoull(value);
            else if (a == "--max-events")
                maximum = std::stoull(value);
            else if (a == "--rate")
                rate = std::stoull(value);
            else
                throw std::runtime_error("unknown argument: " + a);
        }
        if (config.empty())
            throw std::runtime_error("--config is required");
        if (check) {
            char error[512];
            if (fq_check_config(config.c_str(), error, sizeof error) < 0)
                throw std::runtime_error(error);
            std::cout << "Configuration valid (network connectivity not tested).\n";
            return 0;
        }
        std::signal(SIGINT, signal_stop);
        std::signal(SIGTERM, signal_stop);
        Printer printer;
        printer.maximum = maximum;
        printer.rate = rate;
        fq::Client client(&Printer::callback, &printer);
        printer.client = client.native_handle();
        client.open(config);
        auto start = std::chrono::steady_clock::now();
        if (!pcap.empty())
            client.replay(pcap);
        else
            while (!stopped && (!maximum || printer.seen < maximum)) {
                if (duration && std::chrono::steady_clock::now() - start >= std::chrono::seconds(duration))
                    break;
                client.poll(2);
            }
        client.request_stop();
        client.join();
        printer.finish();
        fq_statistics s = {};
        s.struct_size = sizeof s;
        s.abi_version = FQ_ABI_VERSION;
        fq_get_statistics(client.native_handle(), &s);
        std::cerr << "datagrams=" << s.datagrams << " bytes=" << s.bytes << " events=" << s.events
                  << " seen=" << printer.seen << " parse_errors=" << s.parse_errors
                  << " socket_drops=" << s.socket_drops << " truncated_datagrams=" << s.truncated_datagrams
                  << " sampled=" << printer.sampled << " print_queue_overflow=" << printer.overflow << '\n';
        return printer.overflow || s.parse_errors ? 2 : 0;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
