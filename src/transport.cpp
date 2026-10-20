/* UDP 传输层只负责收包和路由，不解析行情协议，也不创建接收线程。
 * 初始化：确认本机网卡 → 按端口建立 socket → 配置 SSM/ASM 加组 → 建立路由表。
 * 接收：poll 等待可读 → recvmsg 取 payload 和辅助信息 → 按组播/端口/源/网卡分流。
 * Client 或外部调用者驱动 poll；回调只借用复用缓冲区，必须同步消费或复制。
 */
#include "internal.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <sstream>
#ifdef __APPLE__
#include <net/if_dl.h>
#endif
namespace fqimpl {
namespace {
void system_error(const std::string &action) {
    throw std::runtime_error(action + ": " + std::strerror(errno));
}
// RAII 保证正常退出和初始化异常时都关闭 socket；内核同时撤销该 socket 的组播成员关系。
struct Socket {
    int fd = -1;
    uint16_t port = 0;
    ~Socket() {
        if (fd >= 0)
            ::close(fd);
    }
};
// group/source 保存网络字节序，与 inet_pton 和 recvmsg 中的地址直接比较；source=0 不限源。
struct Route {
    uint32_t group, source, feed, role, line;
    uint16_t port;
};
uint32_t address(const std::string &s) {
    in_addr a = {};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1)
        throw std::runtime_error("invalid IPv4");
    return a.s_addr;
}
std::string address_text(uint32_t network_order) {
    in_addr value = {};
    value.s_addr = network_order;
    char out[INET_ADDRSTRLEN] = {};
    return inet_ntop(AF_INET, &value, out, sizeof out) ? out : "?";
}
} // namespace
struct Receiver::Impl {
    std::vector<std::unique_ptr<Socket>> sockets;
    std::vector<Route> routes;
    std::vector<pollfd> polls;
    unsigned interface_index = 0, batch = 16;
    // 仅在 FQ_TRACE_TRANSPORT=1 时输出建组信息及前 20 个收包结果，避免诊断日志拖慢热路径。
    bool trace = false;
    unsigned trace_packets_left = 20;
    // 每个 socket 上次看到的内核累计丢包值，用差值累加本轮统计。
    std::vector<uint32_t> overflow;
    // 所有 socket 串行复用一块缓冲区；下一次 recvmsg 会覆盖之前的 payload。
    std::array<uint8_t, 65536> buffer;
    Impl(const Config &config) : batch(config.batch) {
        const char *trace_setting = std::getenv("FQ_TRACE_TRANSPORT");
        trace = trace_setting && std::strcmp(trace_setting, "1") == 0;
        if (config.interface_ip == "0.0.0.0")
            throw std::runtime_error("live reception requires an explicit local interface_ip");
        // 显式选择行情网卡，避免多网卡机器误用默认路由；离线配置不能启动在线接收。
        uint32_t local = address(config.interface_ip);
        ifaddrs *list = 0;
        if (getifaddrs(&list) != 0)
            system_error("getifaddrs");
        for (ifaddrs *p = list; p; p = p->ifa_next)
            if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET &&
                reinterpret_cast<sockaddr_in *>(p->ifa_addr)->sin_addr.s_addr == local)
                interface_index = if_nametoindex(p->ifa_name);
        freeifaddrs(list);
        if (!interface_index)
            throw std::runtime_error("interface_ip is not assigned to a local interface");
        for (const auto &feed : config.feeds)
            for (const auto &e : feed.endpoints) {
                // 同端口的多个组播复用一个 socket，实际角色由目的组播地址路由。
                // 不能只按端口派发，否则同端口的增量/快照/定义会混入同一处理路径。
                Socket *s = 0;
                for (auto &existing : sockets)
                    if (existing->port == e.port)
                        s = existing.get();
                if (!s) {
                    std::unique_ptr<Socket> new_socket(new Socket);
                    new_socket->port = e.port;
                    new_socket->fd = socket(AF_INET, SOCK_DGRAM, 0);
                    if (new_socket->fd < 0)
                        system_error("socket");
                    s = new_socket.get();
                    sockets.push_back(std::move(new_socket));
                    int one = 1;
                    if (setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0)
                        system_error("SO_REUSEADDR");
                    if (setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &config.receive_buffer,
                                   sizeof config.receive_buffer) != 0)
                        system_error("SO_RCVBUF");
#ifdef __linux__
                    // Linux 只接收该 socket 主动加入的组；PKTINFO 提供目的地址及入站网卡，
                    // RXQ_OVFL 提供内核队列溢出计数。macOS 在下方使用不同的辅助消息接口。
                    int zero = 0;
                    if (setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof zero) != 0)
                        system_error("IP_MULTICAST_ALL");
                    if (setsockopt(s->fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one) != 0)
                        system_error("IP_PKTINFO");
                    if (setsockopt(s->fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof one) != 0)
                        system_error("SO_RXQ_OVFL");
#else
                    if (setsockopt(s->fd, IPPROTO_IP, IP_RECVDSTADDR, &one, sizeof one) != 0)
                        system_error("IP_RECVDSTADDR");
                    if (setsockopt(s->fd, IPPROTO_IP, IP_RECVIF, &one, sizeof one) != 0)
                        system_error("IP_RECVIF");
#endif
                    sockaddr_in bind_addr = {};
                    bind_addr.sin_family = AF_INET;
                    bind_addr.sin_port = htons(e.port);
                    bind_addr.sin_addr.s_addr = INADDR_ANY;
                    if (bind(s->fd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof bind_addr) != 0)
                        system_error("bind UDP port " + std::to_string(e.port));
                    int f = fcntl(s->fd, F_GETFL, 0);
                    if (f < 0 || fcntl(s->fd, F_SETFL, f | O_NONBLOCK) < 0)
                        system_error("fcntl O_NONBLOCK");
                    if (fcntl(s->fd, F_SETFD, FD_CLOEXEC) < 0)
                        system_error("FD_CLOEXEC");
                }
                uint32_t group = address(e.group), source = e.source.empty() ? 0 : address(e.source);
                // 配置源 IP 时使用源指定组播；未配置时允许组内任意源。
                // 两种方式都显式指定本机接口，不需要另运行加组脚本或启用混杂模式。
                if (source) {
                    ip_mreq_source req = {};
                    req.imr_multiaddr.s_addr = group;
                    req.imr_interface.s_addr = local;
                    req.imr_sourceaddr.s_addr = source;
                    if (setsockopt(s->fd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &req, sizeof req) != 0)
                        system_error("SSM join " + e.group + " source " + e.source);
                } else {
                    ip_mreq req = {};
                    req.imr_multiaddr.s_addr = group;
                    req.imr_interface.s_addr = local;
                    if (setsockopt(s->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &req, sizeof req) != 0)
                        system_error("ASM join " + e.group);
                }
                Route r = {group, source, feed.id, e.role, e.line, e.port};
                routes.push_back(r);
                if (trace)
                    std::cerr << "[fq transport] join interface_ip=" << config.interface_ip
                              << " ifindex=" << interface_index << " group=" << e.group
                              << " port=" << e.port << " source=" << (e.source.empty() ? "any" : e.source)
                              << " feed=" << feed.id << " role=" << e.role << " line=" << e.line << '\n';
            }
        if (sockets.empty())
            throw std::runtime_error("no endpoints configured for live reception");
        for (auto &s : sockets) {
            pollfd p = {s->fd, POLLIN, 0};
            polls.push_back(p);
            overflow.push_back(0);
        }
    }
    void trace_packet(size_t index, const sockaddr_in &sender, uint32_t destination,
                      unsigned rx_interface, ssize_t size, const char *result, uint32_t feed = 0) {
        if (!trace || !trace_packets_left)
            return;
        --trace_packets_left;
        std::cerr << "[fq transport] recv port=" << sockets[index]->port
                  << " source=" << address_text(sender.sin_addr.s_addr)
                  << " destination=" << (destination ? address_text(destination) : "unknown")
                  << " ifindex=" << rx_interface << " bytes=" << size << " result=" << result;
        if (feed)
            std::cerr << " feed=" << feed;
        std::cerr << '\n';
    }
    void receive(size_t index, const std::function<void(const Received &)> &cb, uint64_t &drops,
                 uint64_t &truncated) {
#ifndef __linux__
        (void)drops;
#endif
        // 一轮最多排空 batch 个包，不等待凑满；限制单个繁忙 socket 对其他端口的占用。
        // 这里是多次非阻塞 recvmsg，不是 recvmmsg，也不做额外的报文批量缓存。
        for (unsigned i = 0; i < batch; ++i) {
            sockaddr_in sender = {};
            alignas(cmsghdr) char control[256] = {};
            iovec io = {buffer.data(), buffer.size()};
            msghdr msg = {};
            msg.msg_name = &sender;
            msg.msg_namelen = sizeof sender;
            msg.msg_iov = &io;
            msg.msg_iovlen = 1;
            msg.msg_control = control;
            msg.msg_controllen = sizeof control;
            ssize_t n = recvmsg(sockets[index]->fd, &msg, MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;
                system_error("recvmsg");
            }
            // 用户态收到报文后的时间，不是网卡硬件时间戳。
            uint64_t timestamp = now_ns();
            // payload 或控制信息截断都不能安全派发：缺少目的组播信息也可能造成错误路由。
            if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
                ++truncated;
                trace_packet(index, sender, 0, 0, n, "truncated");
                continue;
            }
            uint32_t destination = 0;
            unsigned rx_interface = 0;
            for (cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
#ifdef __linux__
                if (h->cmsg_level == IPPROTO_IP && h->cmsg_type == IP_PKTINFO &&
                    h->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
                    in_pktinfo info;
                    std::memcpy(&info, CMSG_DATA(h), sizeof info);
                    destination = info.ipi_addr.s_addr;
                    rx_interface = static_cast<unsigned>(info.ipi_ifindex);
                }
                if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SO_RXQ_OVFL &&
                    h->cmsg_len >= CMSG_LEN(sizeof(uint32_t))) {
                    uint32_t value;
                    std::memcpy(&value, CMSG_DATA(h), 4);
                    // uint32_t 差值按模运算处理累计计数回绕。
                    drops += uint32_t(value - overflow[index]);
                    overflow[index] = value;
                }
#else
                if (h->cmsg_level == IPPROTO_IP && h->cmsg_type == IP_RECVDSTADDR &&
                    h->cmsg_len >= CMSG_LEN(sizeof(in_addr)))
                    std::memcpy(&destination, CMSG_DATA(h), sizeof destination);
                if (h->cmsg_level == IPPROTO_IP && h->cmsg_type == IP_RECVIF &&
                    h->cmsg_len >= CMSG_LEN(sizeof(sockaddr_dl))) {
                    sockaddr_dl info;
                    std::memcpy(&info, CMSG_DATA(h), sizeof info);
                    rx_interface = info.sdl_index;
                }
#endif
            }
            // 普通 UDP recvfrom 只给出发送方，无法区分同端口多组播；这里必须有目的地址。
            if (!destination) {
                trace_packet(index, sender, destination, rx_interface, n, "missing-destination");
                continue;
            }
            if (rx_interface && rx_interface != interface_index) {
                trace_packet(index, sender, destination, rx_interface, n, "wrong-interface");
                continue;
            }
            bool routed = false;
            for (const auto &r : routes)
                if (r.port == sockets[index]->port && r.group == destination &&
                    (!r.source || r.source == sender.sin_addr.s_addr)) {
                    Received d = {r.feed, r.role, r.line, buffer.data(), static_cast<size_t>(n), timestamp};
                    trace_packet(index, sender, destination, rx_interface, n, "routed", r.feed);
                    cb(d);
                    routed = true;
                    break;
                }
            if (!routed)
                trace_packet(index, sender, destination, rx_interface, n, "no-matching-route");
        }
    }
};
Receiver::Receiver(const Config &c) : impl_(new Impl(c)) {}
Receiver::~Receiver() {}
// 仅处理就绪 socket；没有数据时返回，协议超时判断由上层 Client 驱动。
void Receiver::poll(int timeout, const std::function<void(const Received &)> &cb, uint64_t &drops,
                    uint64_t &truncated) {
    int n = ::poll(impl_->polls.data(), static_cast<nfds_t>(impl_->polls.size()), timeout);
    if (n < 0) {
        if (errno == EINTR)
            return;
        system_error("poll");
    }
    if (!n)
        return;
    for (size_t i = 0; i < impl_->polls.size(); ++i) {
        if (impl_->polls[i].revents & (POLLERR | POLLHUP | POLLNVAL))
            throw std::runtime_error("UDP socket poll error");
        if (impl_->polls[i].revents & POLLIN)
            impl_->receive(i, cb, drops, truncated);
    }
}
} // namespace fqimpl
