/* 离线传输适配：经典 PCAP 记录 → Ethernet/VLAN 或原始 IPv4 → UDP payload → Received。
 * 与在线接收共用配置中的组播/端口/源路由和后续协议解码，便于复现恢复问题。
 * 顺序尽快读取，不按录制间隔休眠；不支持 PCAPNG、IP 分片重组，也不验证网络层校验和。
 */
#include "internal.hpp"
#include <fstream>
#include <arpa/inet.h>
namespace fqimpl {
void replay_pcap(const Config &config, const std::string &path,
                 const std::function<void(const Received &)> &callback, const std::function<bool()> &stop) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open PCAP: " + path);
    // 经典 PCAP 全局头固定 24 字节；魔数决定文件头/记录头的字节序和时间小数单位。
    uint8_t header[24];
    if (!f.read(reinterpret_cast<char *>(header), 24))
        throw ParseError("truncated PCAP header");
    Bytes h(header, 24);
    uint32_t magic = h.u32(0);
    bool big = false, nano = false;
    if (magic == 0xa1b2c3d4) {
    } else if (magic == 0xa1b23c4d)
        nano = true;
    else if (magic == 0xd4c3b2a1)
        big = true;
    else if (magic == 0x4d3cb2a1) {
        big = true;
        nano = true;
    } else
        throw ParseError("unsupported PCAP magic (pcapng is not supported)");
    // 这里只转换 PCAP 元数据。Ethernet/IP/UDP 仍按网络大端序读取，
    // UDP 内的 CME/LME 小端字段则交给协议适配器，三层字节序不能混用。
    auto u32 = [&](Bytes b, size_t p) -> uint32_t {
        if (!big)
            return b.u32(p);
        return uint32_t(b.be16(p)) << 16 | b.be16(p + 2);
    };
    auto u16 = [&](Bytes b, size_t p) -> uint16_t { return big ? b.be16(p) : b.u16(p); };
    if (u16(h, 4) != 2 || u16(h, 6) != 4)
        throw ParseError("unsupported PCAP version");
    uint32_t link = u32(h, 20), snaplen = u32(h, 16);
    if (link != 1 && link != 101)
        throw ParseError("PCAP requires Ethernet (1) or raw IPv4 (101)");
    // 复用帧缓冲区；payload 指针只在本次 callback 内有效，下一条记录可能使其失效。
    std::vector<uint8_t> frame;
    frame.reserve(65536);
    struct Route {
        uint32_t group, source, feed, role, line;
        uint16_t port;
    };
    std::vector<Route> routes;
    for (const auto &feed : config.feeds)
        for (const auto &e : feed.endpoints) {
            in_addr g = {}, s = {};
            inet_pton(AF_INET, e.group.c_str(), &g);
            if (!e.source.empty())
                inet_pton(AF_INET, e.source.c_str(), &s);
            Route r = {g.s_addr, s.s_addr, feed.id, e.role, e.line, e.port};
            routes.push_back(r);
        }
    while (!stop()) {
        // 每条记录头为 16 字节，记录“捕获长度”和“原始帧长”；两者不同表示抓包截断。
        uint8_t rec[16];
        f.read(reinterpret_cast<char *>(rec), 16);
        if (f.gcount() == 0 && f.eof())
            break;
        if (f.gcount() != 16)
            throw ParseError("truncated PCAP record header");
        Bytes r(rec, 16);
        uint32_t length = u32(r, 8), original = u32(r, 12), fraction = u32(r, 4);
        if (length > 1048576 || length > snaplen || length > original)
            throw ParseError("invalid PCAP record size");
        if (fraction >= (nano ? 1000000000u : 1000000u))
            throw ParseError("invalid PCAP timestamp fraction");
        frame.resize(length);
        if (!f.read(reinterpret_cast<char *>(frame.data()), length))
            throw ParseError("truncated PCAP data");
        Bytes b(frame.data(), frame.size());
        size_t p = 0;
        if (link == 1) {
            b.require(0, 14);
            uint16_t eth = b.be16(12);
            p = 14;
            // 允许至多两层 VLAN，逐层取内层 EtherType，不能固定假设 IP 从第 14 字节开始。
            unsigned vlan = 0;
            while (eth == 0x8100 || eth == 0x88a8) {
                if (++vlan > 2)
                    throw ParseError("more than two VLAN tags");
                b.require(p, 4);
                eth = b.be16(p + 2);
                p += 4;
            }
            if (eth != 0x0800)
                continue;
        }
        b.require(p, 20);
        if ((b.u8(p) >> 4) != 4 || b.u8(p + 9) != 17)
            continue;
        uint32_t source, dest;
        std::memcpy(&source, b.p + p + 12, 4);
        std::memcpy(&dest, b.p + p + 16, 4);
        // 先过滤无关目的组/源，避免其他业务的分片或截断帧阻碍行情回放。
        // 匹配端口在读出 UDP 头后完成；基础帧结构仍在前面校验。
        bool relevant = false;
        for (const auto &rt : routes)
            if (rt.group == dest && (!rt.source || rt.source == source))
                relevant = true;
        if (!relevant)
            continue;
        if (length != original)
            throw ParseError("target PCAP frame truncated by snaplen");
        // IPv4 IHL 以 4 字节为单位，包含可选字段，不能固定用 20 字节定位 UDP。
        size_t ihl = (b.u8(p) & 15) * 4;
        if (ihl < 20)
            throw ParseError("invalid IPv4 IHL");
        size_t total = b.be16(p + 2);
        if (total < ihl + 8)
            throw ParseError("invalid IPv4 total length");
        b.require(p, total);
        // 掩码包含 MF 和分片偏移；未做重组时不能把任意分片当成完整 UDP payload。
        if (b.be16(p + 6) & 0x3fff)
            throw ParseError("fragmented IPv4 target packet is unsupported");
        size_t udp = p + ihl;
        b.require(udp, 8);
        size_t size = b.be16(udp + 4);
        if (size < 8 || size != total - ihl)
            throw ParseError("invalid UDP length");
        uint16_t port = b.be16(udp + 2);
        for (const auto &rt : routes)
            if (rt.group == dest && rt.port == port && (!rt.source || rt.source == source)) {
                // 统一为 Unix 纳秒，保留录包时间供协议恢复计时使用；不取当前系统时间。
                uint64_t time = uint64_t(u32(r, 0)) * 1000000000ull + uint64_t(fraction) * (nano ? 1 : 1000);
                Received d = {rt.feed, rt.role, rt.line, b.p + udp + 8, size - 8, time};
                callback(d);
                break;
            }
    }
}
} // namespace fqimpl
