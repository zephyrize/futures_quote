#include <futures_quote/client.hpp>
#include "wire_fixture.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

static void count_trade(void *context, const fq_event *event) {
    if (event->type == FQ_TRADE)
        ++*static_cast<uint64_t *>(context);
}

int main(int argc, char **argv) {
    try {
        if (argc < 2 || argc > 3) {
            std::cerr << "fq_bench CONFIG [PACKETS=200000]\nSynthetic CME template 48, one trade/packet, "
                         "synchronous callback.\n";
            return 1;
        }
        uint64_t count = argc == 3 ? std::stoull(argv[2]) : 200000;
        if (!count || count > 10000000)
            throw std::runtime_error("PACKETS must be 1..10000000");
        uint64_t trades = 0;
        fq::Client client(count_trade, &trades);
        client.open(argv[1]);
        V bytes = packet(1, {trade()});
        fq_datagram d = {sizeof(d), FQ_ABI_VERSION, 1, FQ_INCREMENTAL, bytes.data(), bytes.size(),
                         1,         FQ_REPLAYED,    0};
        for (uint32_t i = 1; i <= 10000; ++i) {
            put(bytes, 0, i, 4);
            client.feed(d);
        }
        trades = 0;
        std::vector<uint64_t> latency;
        latency.reserve(static_cast<size_t>(count));
        typedef std::chrono::steady_clock Clock;
        auto begin = Clock::now();
        for (uint32_t i = 0; i < count; ++i) {
            put(bytes, 0, i + 10001, 4);
            auto t = Clock::now();
            client.feed(d);
            latency.push_back(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count()));
        }
        double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        if (trades != count)
            throw std::runtime_error(
                "benchmark callbacks do not match packet count; use config/benchmark.yaml");
        std::sort(latency.begin(), latency.end());
        std::cout << "{\"fixture\":\"synthetic_cme_trade48\",\"packets\":" << count
                  << ",\"callbacks\":" << trades << ",\"packets_per_second\":" << count / seconds
                  << ",\"p50_ns\":" << latency[latency.size() / 2]
                  << ",\"p99_ns\":" << latency[latency.size() * 99 / 100] << ",\"max_ns\":" << latency.back()
                  << "}\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
